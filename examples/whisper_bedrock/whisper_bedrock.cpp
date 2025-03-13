#include "common.h"
#include "common-sdl.h"
#include "whisper.h"
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <iostream>
#include <vector>
#include <deque>
#include <string>
#include <csignal>
#include <memory>
#include <unordered_set>
#include <curl/curl.h>
#include <sstream>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// Remove the old WebSocketPP and standalone ASIO - they conflict with uWebSockets
// #include <websocketpp/config/asio_no_tls.hpp>
// #include <websocketpp/server.hpp>
// #include <functional>
// #include <asio.hpp>

// Include the new WebSocket server header
#include "websocket_server.hpp"

using json = nlohmann::json;

// Forward declarations
class WebSocketServer;

// Constants
const int SAMPLE_RATE = 16000;
const int MAX_AUDIO_BUFFER_SIZE = SAMPLE_RATE * 30; // Max 30 seconds of audio
const int CHUNK_SIZE = SAMPLE_RATE * 5;             // Process 5 seconds at a time

// Global control flags
std::atomic<bool> g_is_running{true};

// Helper function for timing
inline int64_t get_current_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

// VAD parameters
struct VADParams {
    static constexpr float RMS_THRESHOLD = 0.02f;     // Decreased threshold
    static constexpr float PEAK_THRESHOLD = 0.1f;    // Decreased threshold
    static constexpr int SILENCE_MS = 1000;           // 1 second of silence
    static constexpr int MIN_VOICE_MS = 500;          // 500ms minimum voice
};

// Connection mode enum
enum class ConnectionMode {
    SSH,
    WEBSOCKET
};

// AudioCapture class with uWebSockets support
class AudioCapture {
private:
    ConnectionMode mode;
    std::unique_ptr<WebSocketServer> ws_server;
    int network_fd = -1;
    int client_fd = -1;
    static constexpr size_t NETWORK_BUFFER_SIZE = 8192;
    std::vector<float> pending_buffer;
    bool use_network_input;
    std::mutex audio_mutex;
    std::atomic<bool> restart_needed{false};

public:
    AudioCapture() : mode(ConnectionMode::SSH) {}

    bool start(ConnectionMode connection_mode) {
        mode = connection_mode;
        
        switch (mode) {
            case ConnectionMode::SSH:
                fprintf(stdout, "🚀 Starting in SSH mode (raw TCP)\n");
                return start_ssh_mode();
            case ConnectionMode::WEBSOCKET:
                fprintf(stdout, "🚀 Starting in WebSocket mode\n");
                return start_websocket_mode();
            default:
                fprintf(stderr, "❌ Invalid connection mode\n");
                return false;
        }
    }

    ~AudioCapture() {
        stop();
    }

    void stop() {
        if (ws_server) {
            try {
                ws_server->stop();
                ws_server.reset();
            } catch (...) {}
        }

        if (client_fd >= 0) {
            close(client_fd);
            client_fd = -1;
        }
        if (network_fd >= 0) {
            close(network_fd);
            network_fd = -1;
        }
        restart_needed = false;
    }

    std::vector<float> get_audio() {
        switch (mode) {
            case ConnectionMode::SSH:
                return get_ssh_audio();
            case ConnectionMode::WEBSOCKET:
                // Check if WebSocket needs restart
                if (ws_server && !ws_server->is_running()) {
                    restart_needed = true;
                }
                return get_websocket_audio();
            default:
                return std::vector<float>();
        }
    }

    bool needs_restart() const {
        return (mode == ConnectionMode::WEBSOCKET) && 
               (restart_needed || (ws_server && ws_server->needs_restart()));
    }

    // Gets the current WebSocket server (for setting callbacks)
    WebSocketServer* get_websocket_server() {
        return ws_server.get();
    }

    void broadcast_message(const std::string& message) {
        if (ws_server) {
            ws_server->broadcast_text(message);
        }
    }

    ConnectionMode get_mode() const { return mode; }

private:
    bool start_ssh_mode() {
        network_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (network_fd < 0) {
            fprintf(stderr, "❌ Failed to create socket\n");
            return false;
        }

        int opt = 1;
        if (setsockopt(network_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            fprintf(stderr, "❌ Failed to set socket options\n");
            return false;
        }

        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(12345);

        if (bind(network_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            fprintf(stderr, "❌ Failed to bind socket\n");
            return false;
        }

        if (listen(network_fd, 3) < 0) {
            fprintf(stderr, "❌ Failed to listen\n");
            return false;
        }

        return true;
    }

    bool start_websocket_mode() {
        try {
            ws_server = std::make_unique<WebSocketServer>("0.0.0.0", 8765);
            ws_server->set_audio_callback([this](const std::vector<float>& audio) {
                std::lock_guard<std::mutex> lock(audio_mutex);
                pending_buffer.insert(pending_buffer.end(), audio.begin(), audio.end());
            });
            ws_server->start();
            return true;
        } catch (const std::exception& e) {
            fprintf(stderr, "❌ Failed to start WebSocket server: %s\n", e.what());
            return false;
        }
    }

    std::vector<float> get_ssh_audio() {
        if (client_fd < 0) {
            struct sockaddr_in address;
            socklen_t addrlen = sizeof(address);
            client_fd = accept(network_fd, (struct sockaddr *)&address, &addrlen);
            if (client_fd < 0) {
                return std::vector<float>();
            }
            fprintf(stdout, "✅ Client connected\n");
        }

        std::vector<float> audio_data;
        char buffer[NETWORK_BUFFER_SIZE];
        ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);

        if (bytes_read > 0) {
            size_t num_samples = bytes_read / sizeof(float);
            const float* samples = reinterpret_cast<const float*>(buffer);
            audio_data.assign(samples, samples + num_samples);
        } else if (bytes_read <= 0) {
            close(client_fd);
            client_fd = -1;
        }

        return audio_data;
    }

    std::vector<float> get_websocket_audio() {
        std::vector<float> result;
        {
            std::lock_guard<std::mutex> lock(audio_mutex);
            if (!pending_buffer.empty()) {
                result.swap(pending_buffer);
            }
        }
        return result;
    }
};

#ifndef USE_BOOST_BEAST
// Original WebSocketServer implementation (commented out since we're using Boost.Beast version)
/*
class WebSocketServer {
private:
    struct ConnectionState {
        int64_t last_ping_time = 0;
        int64_t last_message_time = 0;
        std::queue<std::vector<float>> audio_queue;
        std::mutex queue_mutex;
        websocketpp::connection_hdl connection;
        std::string contact;
    };

    websocketpp::server<websocketpp::config::asio> server;
    uint16_t port;
    std::string address;
    std::function<void(const std::vector<float>&)> audio_callback;
    std::mutex mtx;
    std::atomic<bool> is_running{true};
    std::unique_ptr<std::thread> server_thread;
    bool server_started{false};
    std::atomic<bool> is_stopping{false};
    std::atomic<int> reconnect_attempts{0};
    static constexpr int MAX_RECONNECT_ATTEMPTS = 5;
    static constexpr int RECONNECT_DELAY_MS = 1000;

    // Thread pool members
    static constexpr size_t MAX_THREADS = 4;
    std::vector<std::thread> worker_threads;
    asio::io_service io_service;
    std::shared_ptr<asio::io_service::work> work;

    std::mutex connections_mutex;
    std::vector<std::shared_ptr<ConnectionState>> connections;

    // Add new members for async operations
    std::shared_ptr<asio::io_service::work> asio_work;
    std::vector<std::thread> asio_threads;
    static constexpr size_t NUM_ASIO_THREADS = 4;

    std::function<void(const std::string&)> contact_update_callback;

    std::atomic<bool> needs_server_restart{false};

public:
    WebSocketServer(const std::string& bind_address = "0.0.0.0", uint16_t bind_port = 8765) 
        : port(bind_port), address(bind_address) {
        init_server();
        init_thread_pool();
    }

    ~WebSocketServer() {
        if (!is_stopping) {
            stop();
        }
    }

    void start() {
        try {
            server.listen(port);
            server.start_accept();
            server_started = true;
            fprintf(stdout, "🌐 WebSocket server listening on ws://%s:%d\n", 
                    address.c_str(), port);
            
            server_thread = std::make_unique<std::thread>([this]() {
                try {
                    while (is_running && !is_stopping) {
                        server.run_one();
                    }
                } catch (const websocketpp::exception& e) {
                    if (is_running && !is_stopping) {
                        fprintf(stderr, "WebSocket error: %s\n", e.what());
                        attempt_reconnect();
                    }
                } catch (const std::exception& e) {
                    if (is_running && !is_stopping) {
                        fprintf(stderr, "Error: %s\n", e.what());
                        attempt_reconnect();
                    }
                }
            });
        } catch (const std::exception& e) {
            fprintf(stderr, "❌ WebSocket server error: %s\n", e.what());
            throw;
        }
    }

    void stop() {
        if (is_stopping) return;
        
        fprintf(stdout, "🛑 Stopping WebSocket server...\n");
        is_stopping = true;
        is_running = false;

        try {
            // First, clear all connections to prevent any new operations
            {
                std::lock_guard<std::mutex> lock(connections_mutex);
                for (auto& conn_state : connections) {
                    try {
                        auto con = server.get_con_from_hdl(conn_state->connection);
                        if (con && con->get_state() == websocketpp::session::state::open) {
                            server.close(conn_state->connection, websocketpp::close::status::going_away, "Server shutdown");
                        }
                    } catch (...) {}
                }
                connections.clear();
            }

            // Stop the ASIO work to allow handlers to complete
            asio_work.reset();
            
            // Stop accepting new connections
            if (server_started) {
                try {
                    server.stop_listening();
                    server.stop();
                } catch (...) {}
                server_started = false;
            }

            // Stop and join ASIO threads
            io_service.stop();
            for (auto& thread : asio_threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            asio_threads.clear();

            // Stop the server thread last
            if (server_thread && server_thread->joinable()) {
                server_thread->join();
                server_thread.reset();
            }

            // Final reset of io_service
            io_service.reset();
            
            fprintf(stdout, "✅ WebSocket server stopped successfully\n");
        } catch (...) {
            fprintf(stderr, "⚠️ Errors occurred during shutdown, but server is stopped\n");
        }
    }

    void set_audio_callback(std::function<void(const std::vector<float>&)> callback) {
        audio_callback = std::move(callback);
    }

    void broadcast_text(const std::string& text) {
        std::lock_guard<std::mutex> lock(connections_mutex);
        for (auto& conn_state : connections) {
            try {
                server.send(conn_state->connection, text, websocketpp::frame::opcode::text);
            } catch (const websocketpp::exception& e) {
                fprintf(stderr, "❌ Failed to send message: %s\n", e.what());
            }
        }
    }

    void set_contact_update_callback(std::function<void(const std::string&)> callback) {
        contact_update_callback = std::move(callback);
    }

    bool needs_restart() const {
        return needs_server_restart.load();
    }

private:
    void init_server() {
        server.clear_access_channels(websocketpp::log::alevel::all);
        server.set_access_channels(websocketpp::log::alevel::connect);
        server.set_access_channels(websocketpp::log::alevel::disconnect);
        server.set_access_channels(websocketpp::log::alevel::app);
        
        server.init_asio();
        server.set_reuse_addr(true);

        // Enable async operations
        server.set_listen_backlog(64); // Increase listen queue
        server.set_open_handler([this](websocketpp::connection_hdl hdl) {
            if (!is_stopping) {
                // Handle new connections asynchronously
                io_service.post([this, hdl]() {
                    handle_open(hdl);
                });
            }
        });

        server.set_close_handler([this](websocketpp::connection_hdl hdl) {
            if (!is_stopping) {
                io_service.post([this, hdl]() {
                    handle_close(hdl);
                });
            }
        });

        server.set_message_handler([this](websocketpp::connection_hdl hdl, websocketpp::server<websocketpp::config::asio>::message_ptr msg) {
            if (!is_stopping) {
                // Process messages asynchronously
                io_service.post([this, hdl, msg]() {
                    handle_message(hdl, msg);
                });
            }
        });

        server.set_fail_handler([this](websocketpp::connection_hdl hdl) {
            if (!is_stopping) {
                io_service.post([this, hdl]() {
                    fprintf(stderr, "❌ Connection failed\n");
                    try {
                        auto con = server.get_con_from_hdl(hdl);
                        fprintf(stderr, "Failure reason: %s\n", con->get_ec().message().c_str());
                        handle_close(hdl);
                    } catch (const std::exception& e) {
                        fprintf(stderr, "Error getting failure info: %s\n", e.what());
                    }
                });
            }
        });

        // Start ASIO thread pool
        asio_work = std::make_shared<asio::io_service::work>(io_service);
        for (size_t i = 0; i < NUM_ASIO_THREADS; ++i) {
            asio_threads.emplace_back([this]() {
                try {
                    io_service.run();
                } catch (const std::exception& e) {
                    fprintf(stderr, "ASIO thread error: %s\n", e.what());
                }
            });
        }
    }

    void init_thread_pool() {
        work = std::make_shared<asio::io_service::work>(io_service);
        for (size_t i = 0; i < MAX_THREADS; ++i) {
            worker_threads.emplace_back([this]() {
                io_service.run();
            });
        }
    }

    void handle_message(websocketpp::connection_hdl hdl, websocketpp::server<websocketpp::config::asio>::message_ptr msg) {
        try {
            auto conn_state = get_connection_state(hdl);
            if (!conn_state) {
                // Create new connection state if it doesn't exist
                // This handles the case where a message comes in before the open handler is called
                std::lock_guard<std::mutex> lock(connections_mutex);
                conn_state = std::make_shared<ConnectionState>();
                conn_state->connection = hdl;
                conn_state->last_ping_time = get_current_time_ms();
                conn_state->last_message_time = get_current_time_ms();
                connections.push_back(conn_state);
                fprintf(stdout, "✅ WebSocket client connected via message handler (Total connections: %zu)\n", 
                        connections.size());
            }

            if (msg->get_opcode() == websocketpp::frame::opcode::text) {
                // Handle configuration message
                json data = json::parse(msg->get_payload());
                if (data["type"] == "config") {
                    conn_state->contact = data["contact"];
                    fprintf(stdout, "📱 Received contact configuration: %s\n", 
                            conn_state->contact.c_str());
                    
                    // Notify listeners of new contact
                    if (!conn_state->contact.empty() && contact_update_callback) {
                        contact_update_callback(conn_state->contact);
                    }
                }
            } else {
                // Handle binary audio data
                const std::string& payload = msg->get_payload();
                std::vector<float> audio_data(
                    reinterpret_cast<const float*>(payload.data()),
                    reinterpret_cast<const float*>(payload.data() + payload.size())
                );

                if (audio_callback) {
                    audio_callback(audio_data);
                }
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "❌ Error handling message: %s\n", e.what());
        }
    }

    void handle_open(websocketpp::connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connections_mutex);
        auto conn_state = std::make_shared<ConnectionState>();
        conn_state->connection = hdl;
        conn_state->last_ping_time = get_current_time_ms();
        conn_state->last_message_time = get_current_time_ms();
        connections.push_back(conn_state);
        reconnect_attempts = 0;
        fprintf(stdout, "✅ WebSocket client connected (Total connections: %zu)\n", 
                connections.size());
    }

    void handle_close(websocketpp::connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connections_mutex);
        connections.erase(
            std::remove_if(connections.begin(), connections.end(),
                [hdl](const std::shared_ptr<ConnectionState>& state) {
                    return !state->connection.owner_before(hdl) && 
                           !hdl.owner_before(state->connection);
                }),
            connections.end()
        );
        fprintf(stdout, "🔌 WebSocket client disconnected (Remaining connections: %zu)\n", 
                connections.size());

        if (connections.empty() && !is_stopping) {
            attempt_reconnect();
        }
    }

    std::shared_ptr<ConnectionState> get_connection_state(websocketpp::connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connections_mutex);
        auto it = std::find_if(connections.begin(), connections.end(),
            [hdl](const std::shared_ptr<ConnectionState>& state) {
                return !state->connection.owner_before(hdl) && 
                       !hdl.owner_before(state->connection);
            });
        return (it != connections.end()) ? *it : nullptr;
    }

    void restart_server() {
        try {
            // First, set flags to ensure no processing during cleanup
            is_stopping = true;
            
            // Completely clean up existing resources
            fprintf(stdout, "🔄 Cleaning up server resources before restart...\n");
            
            try {
                // Clear all handlers to prevent callbacks during shutdown
                server.set_open_handler([](websocketpp::connection_hdl){});
                server.set_close_handler([](websocketpp::connection_hdl){});
                server.set_message_handler([](websocketpp::connection_hdl, websocketpp::server<websocketpp::config::asio>::message_ptr){});
                server.set_fail_handler([](websocketpp::connection_hdl){});
                
                // Close all connections explicitly
                {
                    std::lock_guard<std::mutex> lock(connections_mutex);
                    for (auto& conn : connections) {
                        try {
                            auto wscon = server.get_con_from_hdl(conn->connection);
                            if (wscon && wscon->get_state() == websocketpp::session::state::open) {
                                server.close(conn->connection, websocketpp::close::status::going_away, "Server restart");
                            }
                        } catch (...) {}
                    }
                    connections.clear();
                }
                
                // Stop the server
                if (server_started) {
                    try {
                        server.stop_listening();
                        server.stop();
                    } catch (...) {}
                    server_started = false;
                }
                
                // Join server thread if running
                if (server_thread && server_thread->joinable()) {
                    server_thread->join();
                    server_thread.reset();
                }
                
                // Complete cleanup delay
                std::this_thread::sleep_for(std::chrono::seconds(1));
                
            } catch (const std::exception& e) {
                fprintf(stderr, "Non-fatal error during cleanup: %s\n", e.what());
            }
            
            // Signal main application that a complete restart is needed
            needs_server_restart = true;
            fprintf(stdout, "⚠️ Server needs complete restart from main application\n");
            
            // Reset state
            is_stopping = false;
            is_running = false;  // Tell main loop we're not running
            
        } catch (const std::exception& e) {
            fprintf(stderr, "❌ Fatal error during server shutdown: %s\n", e.what());
            needs_server_restart = true;
        }
    }

    void attempt_reconnect() {
        if (reconnect_attempts >= MAX_RECONNECT_ATTEMPTS || is_stopping) {
            fprintf(stderr, "❌ Max reconnection attempts reached or server is stopping\n");
            needs_server_restart = true;
            return;
        }

        reconnect_attempts++;
        fprintf(stdout, "🔄 Attempting to reconnect (attempt %d/%d)...\n", 
                reconnect_attempts.load(), MAX_RECONNECT_ATTEMPTS);

        // Use exponential backoff for retry delays
        int delay = RECONNECT_DELAY_MS * (1 << (reconnect_attempts - 1));
        
        // Use a separate thread for reconnection to avoid blocking
        std::thread([this, delay]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            try {
                if (!is_stopping) {
                    restart_server();
                }
            } catch (const std::exception& e) {
                fprintf(stderr, "❌ Reconnection attempt failed: %s\n", e.what());
                needs_server_restart = true;
            }
        }).detach();
    }
};
*/
#endif

// Bedrock processor implementation
class BedrockProcessor {
private:
    CURL* curl;
    std::string contact;
    static constexpr int MAX_RETRIES = 3;
    static constexpr int RETRY_DELAY_MS = 1000;

public:
    BedrockProcessor(const std::string& contact_number) : contact(contact_number) {
        curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }
    }

    ~BedrockProcessor() {
        if (curl) {
            curl_easy_cleanup(curl);
        }
    }

    std::string process_text(const std::string& text) {
        for (int retry = 0; retry < MAX_RETRIES; retry++) {
            try {
                std::string response = make_request(text);
                if (!response.empty() && response.find("Error") == std::string::npos) {
                    return response;
                }
                
                fprintf(stderr, "\n⚠️ Retry %d/%d: %s\n", 
                    retry + 1, MAX_RETRIES, response.c_str());
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(RETRY_DELAY_MS * (retry + 1))
                );
            } catch (const std::exception& e) {
                fprintf(stderr, "\n❌ Request failed: %s\n", e.what());
            }
        }
        return "Error: Maximum retries exceeded";
    }

    void set_contact(const std::string& new_contact) {
        contact = new_contact;
    }

private:
    std::string make_request(const std::string& text) {
        if (!curl) {
            return "Error: CURL not initialized";
        }

        json payload = {
            {"message", text},
            {"contact", contact}
        };

        std::string url = "https://jkvic2hlwcrusijwsm4uzqn4je0kafwc.lambda-url.us-east-2.on.aws/llm";
        std::string response_string;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        std::string payload_str = payload.dump();
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str.c_str());

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);

        if (res != CURLE_OK) {
            return std::string("Error: ") + curl_easy_strerror(res);
        }

        try {
            json response = json::parse(response_string);
            return response["response"];
        } catch (const std::exception& e) {
            return std::string("Error parsing response: ") + e.what();
        }
    }

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }
};

// Audio processor implementation
class AudioProcessor {
private:
    struct whisper_context* ctx;
    AudioCapture& audio_capture;
    bool enable_timestamps;
    std::unique_ptr<BedrockProcessor> bedrock_processor;
    std::vector<float> current_buffer;
    std::string accumulated_transcription;
    int64_t last_voice_activity_time = 0;

public:
    AudioProcessor(const char* model_path, AudioCapture& capture, 
                  bool timestamps, const std::string& contact_number) 
        : audio_capture(capture), enable_timestamps(timestamps) {
        
        whisper_context_params params = whisper_context_default_params();
        params.use_gpu = true;
        
        ctx = whisper_init_from_file_with_params(model_path, params);
        if (!ctx) {
            throw std::runtime_error("Failed to initialize whisper context");
        }

        // Initialize Bedrock processor if contact number is provided
        if (!contact_number.empty()) {
            bedrock_processor = std::make_unique<BedrockProcessor>(contact_number);
        }
    }

    ~AudioProcessor() {
        if (ctx) {
            whisper_free(ctx);
        }
    }

    void process_audio(const std::vector<float>& audio, const std::string& contact) {
        // Initialize Bedrock processor if we have a contact but no processor
        if (!contact.empty() && !bedrock_processor) {
            fprintf(stdout, "🔄 Auto-initializing Bedrock processor with contact: %s\n", contact.c_str());
            bedrock_processor = std::make_unique<BedrockProcessor>(contact);
        }
        
        // Update contact in case it changed
        if (bedrock_processor && !contact.empty()) {
            bedrock_processor->set_contact(contact);
        }

        current_buffer.insert(current_buffer.end(), audio.begin(), audio.end());

        if (current_buffer.size() >= CHUNK_SIZE) {
            float rms = 0.0f;
            float peak = 0.0f;
            for (float sample : current_buffer) {
                rms += sample * sample;
                peak = std::max(peak, std::abs(sample));
            }
            rms = std::sqrt(rms / current_buffer.size());

            bool is_voice = rms > VADParams::RMS_THRESHOLD || peak > VADParams::PEAK_THRESHOLD;
            int64_t current_time = get_current_time_ms();

            if (is_voice) {
                last_voice_activity_time = current_time;
            } else if (current_time - last_voice_activity_time > VADParams::SILENCE_MS && 
                      !current_buffer.empty()) {
                whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
                wparams.print_progress = false;
                wparams.print_special = false;
                wparams.print_realtime = false;
                wparams.print_timestamps = enable_timestamps;
                wparams.single_segment = true;
                wparams.max_tokens = 32;
                wparams.language = "en";

                if (whisper_full(ctx, wparams, current_buffer.data(), current_buffer.size()) == 0) {
                    int result_len = whisper_full_n_segments(ctx);
                    for (int i = 0; i < result_len; ++i) {
                        const char* text = whisper_full_get_segment_text(ctx, i);
                        accumulated_transcription += text;
                    }
                    output_transcription(contact);
                }
                current_buffer.clear();
            }
        }
    }

    void initialize_bedrock(const std::string& contact_number) {
        if (contact_number.empty()) return;
        
        if (!bedrock_processor) {
            fprintf(stdout, "🔧 Initializing Bedrock processor with contact: %s\n", contact_number.c_str());
            bedrock_processor = std::make_unique<BedrockProcessor>(contact_number);
        } else {
            fprintf(stdout, "🔄 Updating Bedrock processor contact to: %s\n", contact_number.c_str());
            bedrock_processor->set_contact(contact_number);
        }
    }

private:
    void output_transcription(const std::string& contact) {
        if (!accumulated_transcription.empty()) {
            // Print debug info
            fprintf(stdout, "\n🔍 Debug - Received text: '%s'\n", accumulated_transcription.c_str());
            
            // Trim the text for comparison
            std::string trimmed_text = accumulated_transcription;
            // Trim leading whitespace
            trimmed_text.erase(0, trimmed_text.find_first_not_of(" \t\n\r"));
            // Trim trailing whitespace
            trimmed_text.erase(trimmed_text.find_last_not_of(" \t\n\r") + 1);
            
            // Skip if ONLY silence or blank audio (more robust check)
            if (trimmed_text == "[BLANK_AUDIO]" || 
                trimmed_text == "[ Silence ]" ||
                trimmed_text.find("[BLANK_AUDIO]") != std::string::npos ||
                trimmed_text.find("[ Silence ]") != std::string::npos ||
                trimmed_text.find("[Silence]") != std::string::npos) {
                fprintf(stdout, "⏭️ Skipping silence marker: '%s'\n", trimmed_text.c_str());
                accumulated_transcription.clear();
                return;
            }

            // Create a JSON response
            json response = {
                {"type", "transcription"},
                {"text", accumulated_transcription}
            };
            
            fprintf(stdout, "\n📝 Full Transcription:\n%s\n", accumulated_transcription.c_str());
            
            if (audio_capture.get_mode() == ConnectionMode::WEBSOCKET) {
                audio_capture.broadcast_message(response.dump());
            }
            
            // Always try to process with Bedrock if available
            if (bedrock_processor) {
                try {
                    fprintf(stdout, "\n🤖 Processing with Bedrock...\n");
                    bedrock_processor->set_contact(contact);
                    fprintf(stdout, "📞 Using contact: %s\n", contact.c_str());
                    
                    std::string ai_response = bedrock_processor->process_text(accumulated_transcription);
                    
                    fprintf(stdout, "📣 Got response: '%s'\n", ai_response.c_str());
                    
                    if (!ai_response.empty() && ai_response.find("Error") == std::string::npos) {
                        fprintf(stdout, "\n💬 AI Response:\n%s\n", ai_response.c_str());
                        
                        if (audio_capture.get_mode() == ConnectionMode::WEBSOCKET) {
                            json ai_msg = {
                                {"type", "ai_response"},
                                {"text", ai_response}
                            };
                            audio_capture.broadcast_message(ai_msg.dump());
                        }
                    }
                } catch (const std::exception& e) {
                    fprintf(stderr, "\n❌ Error processing with Bedrock: %s\n", e.what());
                }
            } else {
                fprintf(stderr, "\n⚠️ No Bedrock processor available\n");
            }
            accumulated_transcription.clear();
        }
    }
};

// Signal handler
void signal_handler(int) {
    g_is_running = false;
}

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        fprintf(stdout, R"(
Whisper Bedrock WebSocket Service - Real-time Speech Recognition with Bedrock AI

Usage: %s <model_path> [options]

Required:
    <model_path>            Path to the Whisper model file (e.g., models/ggml-base.en.bin)

Options:
    --mode <mode>          Connection mode: 'websocket' or 'ssh' (default: ssh)
    --port <port>          Port number (default: 8765 for WebSocket, 12345 for SSH)
    --address <addr>       Bind address (default: 0.0.0.0)
    --timestamps           Enable timestamps in transcription
    --contact <number>     Contact number for Bedrock API
    -h, --help            Show this help message

Examples:
    # Start WebSocket server with default settings
    %s models/ggml-base.en.bin --mode websocket

    # Start SSH server on custom port with contact number
    %s models/ggml-base.en.bin --mode ssh --port 9000 --contact 8000000706

    # Start WebSocket server with all options
    %s models/ggml-base.en.bin --mode websocket --port 8765 --address 0.0.0.0 --timestamps --contact 8000000706

)", argv[0], argv[0], argv[0]);
        return 0;
    }

    bool enable_timestamps = false;
    std::string contact_number;
    ConnectionMode mode = ConnectionMode::SSH;
    uint16_t port = 8765;
    std::string bind_address = "0.0.0.0";
    
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--timestamps") {
            enable_timestamps = true;
        }
        else if (arg == "--contact" && i + 1 < argc) {
            contact_number = argv[++i];
        }
        else if (arg == "--mode" && i + 1 < argc) {
            std::string mode_str = argv[++i];
            if (mode_str == "ssh") {
                mode = ConnectionMode::SSH;
                port = 12345;
            }
            else if (mode_str == "websocket") mode = ConnectionMode::WEBSOCKET;
            else {
                fprintf(stderr, "❌ Invalid mode: %s (must be 'ssh' or 'websocket')\n", 
                        mode_str.c_str());
                return 1;
            }
        }
        else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        }
        else if (arg == "--address" && i + 1 < argc) {
            bind_address = argv[++i];
        }
    }

    signal(SIGINT, signal_handler);
    std::unique_ptr<AudioCapture> capture;

    try {
        std::atomic<bool> shutdown_complete{false};
        std::thread shutdown_thread;

        auto cleanup = [&]() {
            fprintf(stdout, "\n👋 Stopping audio capture...\n");
            if (capture) {
                capture->stop();
                capture.reset();
            }
            shutdown_complete = true;
        };

        signal(SIGINT, [](int) {
            static int count = 0;
            count++;
            
            if (count == 1) {
                fprintf(stdout, "\nReceived Ctrl+C, stopping gracefully...\n");
                g_is_running = false;
            } else {
                fprintf(stdout, "\nForced exit.\n");
                exit(1);
            }
        });

        fprintf(stdout, "🚀 Starting Whisper.cpp in %s mode\n", 
                mode == ConnectionMode::SSH ? "SSH" : "WebSocket");
        
        if (mode == ConnectionMode::WEBSOCKET) {
            fprintf(stdout, "📡 WebSocket endpoint will be: ws://%s:%d\n", 
                                bind_address.c_str(), port);
        } else {
            fprintf(stdout, "📡 TCP endpoint will be: %s:%d\n", 
                    bind_address.c_str(), port);
        }
        
        capture = std::make_unique<AudioCapture>();
        if (!capture->start(mode)) {
            fprintf(stderr, "❌ Failed to start audio capture\n");
            return 1;
        }

        AudioProcessor processor(argv[1], *capture, enable_timestamps, contact_number);

        std::string current_contact = contact_number;
        if (capture->get_mode() == ConnectionMode::WEBSOCKET) {
            dynamic_cast<WebSocketServer*>(capture->get_websocket_server())->set_contact_update_callback(
                [&processor, &current_contact](const std::string& new_contact) {
                    current_contact = new_contact;
                    processor.initialize_bedrock(new_contact);
                }
            );
        }

        fprintf(stdout, "\n🎙️  Listening for audio... Press Ctrl+C to stop\n");

        // Main application loop with server restart logic
        bool needs_server_restart = false;
        
        while (g_is_running) {
            // Check if we need to restart the server
            if (mode == ConnectionMode::WEBSOCKET && 
                capture && capture->needs_restart()) {
                
                fprintf(stdout, "🔄 Detected server needs restart...\n");
                
                // Save current contact for after restart
                std::string saved_contact = current_contact;
                
                // Complete shutdown of current capture
                capture->stop();
                capture.reset();
                
                // Wait a moment to ensure complete cleanup
                std::this_thread::sleep_for(std::chrono::seconds(1));
                
                // Create a fresh instance
                fprintf(stdout, "🔄 Creating new WebSocket server...\n");
                capture = std::make_unique<AudioCapture>();
                
                if (!capture->start(mode)) {
                    fprintf(stderr, "❌ Failed to restart audio capture\n");
                    return 1;
                }
                
                // Restore the contact configuration
                current_contact = saved_contact;
                
                // Set up callback for contact updates
                if (!current_contact.empty()) {
                    processor.initialize_bedrock(current_contact);
                }
                
                if (capture->get_mode() == ConnectionMode::WEBSOCKET) {
                    dynamic_cast<WebSocketServer*>(capture->get_websocket_server())->set_contact_update_callback(
                        [&processor, &current_contact](const std::string& new_contact) {
                            current_contact = new_contact;
                            processor.initialize_bedrock(new_contact);
                        }
                    );
                }
                
                fprintf(stdout, "✅ Server successfully restarted!\n");
            }
            
            // Process audio as usual
            auto audio = capture->get_audio();
            if (!audio.empty()) {
                processor.process_audio(audio, current_contact);
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Start cleanup with timeout
        shutdown_thread = std::thread([&]() {
            cleanup();
        });

        // Wait for cleanup with timeout
        auto start = std::chrono::steady_clock::now();
        while (!shutdown_complete) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
                fprintf(stderr, "\n❌ Shutdown timeout, forcing exit...\n");
                exit(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (shutdown_thread.joinable()) {
            shutdown_thread.join();
        }

        return 0;

    } catch (const std::exception& e) {
        fprintf(stderr, "❌ Error: %s\n", e.what());
        if (capture) {
            capture->stop();
            capture.reset();
        }
        return 1;
    }
}