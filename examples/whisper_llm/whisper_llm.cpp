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
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <functional>
#include <asio.hpp>

using json = nlohmann::json;
using wsserver = websocketpp::server<websocketpp::config::asio>;

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

// Define WebSocketServer first
class WebSocketServer {
private:
    // Define ConnectionState struct first
    struct ConnectionState {
        int64_t last_ping_time = 0;
        int64_t last_message_time = 0;
        std::queue<std::vector<float>> audio_queue;
        std::mutex queue_mutex;
        websocketpp::connection_hdl connection;  // Store the handle directly
    };

    wsserver server;
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

    // Connection management with vector instead of unordered_map
    std::mutex connections_mutex;
    std::vector<std::shared_ptr<ConnectionState>> connections;

public:
    WebSocketServer(const std::string& bind_address = "0.0.0.0", uint16_t bind_port = 8765) 
        : port(bind_port), address(bind_address) {
        
        init_server();
        init_thread_pool();
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
        is_stopping = true;
        is_running = false;

        try {
            // Close all connections first
            {
                std::lock_guard<std::mutex> lock(mtx);
                for (auto& hdl : connections) {
                    try {
                        auto con = server.get_con_from_hdl(hdl->connection);
                        if (con && con->get_state() == websocketpp::session::state::open) {
                            server.close(hdl->connection, websocketpp::close::status::going_away, "Server shutdown");
                        }
                    } catch (...) {}
                }
                connections.clear();
            }

            // Stop the server if it was started
            if (server_started) {
                try {
                    server.stop_listening();
                    server.stop();
                } catch (...) {}
            }

            // Wait for server thread to finish
            if (server_thread && server_thread->joinable()) {
                server_thread->join();
            }

        } catch (...) {
            // Ignore all exceptions during shutdown
        }
    }

    ~WebSocketServer() {
        if (!is_stopping) {
            stop();
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

private:
    void init_server() {
        // Set logging settings
        server.clear_access_channels(websocketpp::log::alevel::all);
        server.set_access_channels(websocketpp::log::alevel::connect);
        server.set_access_channels(websocketpp::log::alevel::disconnect);
        server.set_access_channels(websocketpp::log::alevel::app);
        
        // Initialize ASIO
        server.init_asio();
        server.set_reuse_addr(true);

        // Register handlers
        server.set_message_handler([this](websocketpp::connection_hdl hdl, wsserver::message_ptr msg) {
            if (!is_stopping) handle_message(hdl, msg);
        });

        server.set_open_handler([this](websocketpp::connection_hdl hdl) {
            if (!is_stopping) {
                handle_open(hdl);
            }
        });

        server.set_close_handler([this](websocketpp::connection_hdl hdl) {
            if (!is_stopping) {
                handle_close(hdl);
            }
        });

        server.set_fail_handler([this](websocketpp::connection_hdl hdl) {
            if (!is_stopping) {
                fprintf(stderr, "❌ Connection failed\n");
                try {
                    auto con = server.get_con_from_hdl(hdl);
                    fprintf(stderr, "Failure reason: %s\n", con->get_ec().message().c_str());
                    handle_close(hdl);
                } catch (const std::exception& e) {
                    fprintf(stderr, "Error getting failure info: %s\n", e.what());
                }
            }
        });
    }

    void handle_open(websocketpp::connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connections_mutex);
        auto conn_state = std::make_shared<ConnectionState>();
        conn_state->connection = hdl;
        conn_state->last_ping_time = get_current_time_ms();
        conn_state->last_message_time = get_current_time_ms();
        connections.push_back(conn_state);
        reconnect_attempts = 0; // Reset reconnect attempts on successful connection
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

        // Attempt reconnection if no connections remain
        if (connections.empty() && !is_stopping) {
            attempt_reconnect();
        }
    }

    void attempt_reconnect() {
        if (reconnect_attempts >= MAX_RECONNECT_ATTEMPTS) {
            fprintf(stderr, "❌ Max reconnection attempts reached\n");
            return;
        }

        reconnect_attempts++;
        fprintf(stdout, "🔄 Attempting to reconnect (attempt %d/%d)...\n", 
                reconnect_attempts.load(), MAX_RECONNECT_ATTEMPTS);

        // Restart the server with exponential backoff
        std::thread([this]() {
            try {
                int delay = RECONNECT_DELAY_MS * (1 << (reconnect_attempts - 1));
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                
                if (!is_stopping) {
                    restart_server();
                }
            } catch (const std::exception& e) {
                fprintf(stderr, "❌ Reconnection attempt failed: %s\n", e.what());
            }
        }).detach();
    }

    void restart_server() {
        try {
            // Stop current server
            if (server_started) {
                server.stop_listening();
                server.stop();
            }

            // Reinitialize server
            init_server();
            
            // Start listening again
            server.listen(port);
            server.start_accept();
            
            fprintf(stdout, "🌐 WebSocket server restarted and listening on ws://%s:%d\n", 
                    address.c_str(), port);
            
            server_started = true;
        } catch (const std::exception& e) {
            fprintf(stderr, "❌ Server restart failed: %s\n", e.what());
        }
    }

    void handle_message(websocketpp::connection_hdl hdl, wsserver::message_ptr msg) {
        // Process message asynchronously
        io_service.post([this, hdl, msg]() {
            try {
                if (msg->get_opcode() == websocketpp::frame::opcode::binary) {
                    auto conn_state = get_connection_state(hdl);
                    if (!conn_state) return;

                    const std::string& payload = msg->get_payload();
                    const float* audio_data = reinterpret_cast<const float*>(payload.data());
                    size_t num_samples = payload.size() / sizeof(float);

                    if (audio_callback && num_samples > 0) {
                        std::vector<float> audio_buffer(audio_data, audio_data + num_samples);
                        
                        // Queue audio data for processing
                        {
                            std::lock_guard<std::mutex> lock(conn_state->queue_mutex);
                            conn_state->audio_queue.push(std::move(audio_buffer));
                        }
                        
                        // Process audio in chunks
                        process_audio_queue(hdl);
                    }

                    // Update last message time
                    conn_state->last_message_time = get_current_time_ms();
                }
            } catch (const std::exception& e) {
                fprintf(stderr, "Error processing message: %s\n", e.what());
            }
        });
    }

    void process_audio_queue(websocketpp::connection_hdl hdl) {
        auto conn_state = get_connection_state(hdl);
        if (!conn_state) return;

        std::vector<float> audio_chunk;
        
        {
            std::lock_guard<std::mutex> lock(conn_state->queue_mutex);
            while (!conn_state->audio_queue.empty()) {
                auto& current = conn_state->audio_queue.front();
                audio_chunk.insert(audio_chunk.end(), 
                                 current.begin(), 
                                 current.end());
                conn_state->audio_queue.pop();
                
                // Process in chunks to avoid memory issues
                if (audio_chunk.size() >= CHUNK_SIZE) {
                    if (audio_callback) {
                        audio_callback(audio_chunk);
                    }
                    audio_chunk.clear();
                }
            }
        }

        // Process remaining audio
        if (!audio_chunk.empty() && audio_callback) {
            audio_callback(audio_chunk);
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

    void monitor_connections() {
        while (is_running && !is_stopping) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            std::lock_guard<std::mutex> lock(connections_mutex);
            int64_t current_time = get_current_time_ms();
            
            // Check connection health
            for (auto it = connections.begin(); it != connections.end();) {
                auto& state = *it;
                if (current_time - state->last_message_time > 60000) { // 60 seconds timeout
                    try {
                        server.close(state->connection, 
                                   websocketpp::close::status::going_away,
                                   "Connection timeout");
                    } catch (...) {}
                    it = connections.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    void init_thread_pool() {
        // Initialize thread pool for connection handling
        work = std::make_shared<asio::io_service::work>(io_service);
        
        for (size_t i = 0; i < MAX_THREADS; ++i) {
            worker_threads.emplace_back([this]() {
                try {
                    io_service.run();
                } catch (const std::exception& e) {
                    fprintf(stderr, "Worker thread error: %s\n", e.what());
                }
            });
        }
    }
};

// Then define AudioCapture
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
        if (ws_server) {
            ws_server->stop();
        }
        if (client_fd >= 0) close(client_fd);
        if (network_fd >= 0) close(network_fd);
    }

    void stop() {
        if (ws_server) {
            try {
                ws_server->stop();
                ws_server.reset();
            } catch (...) {
                // Ignore shutdown errors
            }
        }

        if (client_fd >= 0) {
            close(client_fd);
            client_fd = -1;
        }
        if (network_fd >= 0) {
            close(network_fd);
            network_fd = -1;
        }
    }

    std::vector<float> get_audio() {
        switch (mode) {
            case ConnectionMode::SSH:
                return get_ssh_audio();
            case ConnectionMode::WEBSOCKET:
                return get_websocket_audio();
            default:
                return std::vector<float>();
        }
    }

    ConnectionMode get_mode() const { return mode; }
    
    void broadcast_message(const std::string& message) {
        if (ws_server) {
            ws_server->broadcast_text(message);
        }
    }

private:
    // SSH (raw TCP) mode implementation
    bool start_ssh_mode() {
        network_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (network_fd < 0) {
            fprintf(stderr, "❌ Failed to create socket\n");
            return false;
        }

        // Enable socket reuse
        int reuse = 1;
        if (setsockopt(network_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
            fprintf(stderr, "❌ Failed to set socket options\n");
            close(network_fd);
            return false;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(12345);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);  // Listen on all interfaces

        if (bind(network_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "❌ Failed to bind socket: %s\n", strerror(errno));
            close(network_fd);
            return false;
        }

        if (listen(network_fd, 1) < 0) {
            fprintf(stderr, "❌ Failed to listen on socket\n");
            close(network_fd);
            return false;
        }

        fprintf(stdout, "🎧 Listening for audio on port 12345...\n");
        
        // Accept client connection
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        fprintf(stdout, "Waiting for client connection...\n");
        
        client_fd = accept(network_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            fprintf(stderr, "❌ Failed to accept client connection\n");
            close(network_fd);
            return false;
        }

        // Set socket to non-blocking mode
        int flags = fcntl(network_fd, F_GETFL, 0);
        fcntl(network_fd, F_SETFL, flags | O_NONBLOCK);
        
        // Also set client socket to non-blocking after accept
        if (client_fd >= 0) {
            flags = fcntl(client_fd, F_GETFL, 0);
            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
        }

        fprintf(stdout, "✅ Client connected!\n");
        return true;
    }

    // WebSocket mode implementation
    bool start_websocket_mode() {
        try {
            ws_server = std::make_unique<WebSocketServer>("0.0.0.0", 8765);
            ws_server->set_audio_callback([this](const std::vector<float>& audio) {
                process_websocket_audio(audio);
            });
            
            std::thread([this]() {
                ws_server->start();
            }).detach();

            return true;
        } catch (const std::exception& e) {
            fprintf(stderr, "❌ Failed to start WebSocket server: %s\n", e.what());
            return false;
        }
    }

    std::vector<float> get_ssh_audio() {
        if (client_fd < 0) {
            if (!handle_client_disconnect()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                return std::vector<float>();
            }
        }

        std::vector<uint8_t> raw_buffer(NETWORK_BUFFER_SIZE);
        ssize_t bytes_read = recv(client_fd, raw_buffer.data(), raw_buffer.size(), MSG_DONTWAIT);
        
        if (bytes_read <= 0) {
            if (bytes_read == 0 || errno == ECONNRESET || errno == EPIPE || errno == ECONNREFUSED) {
                // Client disconnected or connection error
                handle_client_disconnect();
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                fprintf(stderr, "❌ Read error: %s\n", strerror(errno));
            }
            return std::vector<float>();
        }

        // Process received data
        size_t num_samples = bytes_read / sizeof(float);
        std::vector<float> audio_buffer(num_samples);
        memcpy(audio_buffer.data(), raw_buffer.data(), bytes_read);
        
        return audio_buffer;
    }

    void process_websocket_audio(const std::vector<float>& audio) {
        // Process the received audio data
        std::lock_guard<std::mutex> lock(audio_mutex);
        pending_buffer.insert(pending_buffer.end(), audio.begin(), audio.end());
    }

    std::vector<float> get_websocket_audio() {
        std::lock_guard<std::mutex> lock(audio_mutex);
        std::vector<float> result;
        if (!pending_buffer.empty()) {
            result.swap(pending_buffer);
        }
        return result;
    }

    bool handle_client_disconnect() {
        if (client_fd >= 0) {
            close(client_fd);
            client_fd = -1;
            fprintf(stdout, "\n🔌 Client disconnected. Waiting for new connection...\n");
            
            // Clear any pending buffers
            pending_buffer.clear();
        }
        
        // Wait for new client
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        // Set accept to non-blocking
        int flags = fcntl(network_fd, F_GETFL, 0);
        fcntl(network_fd, F_SETFL, flags | O_NONBLOCK);
        
        fprintf(stdout, "Waiting for client connection on port 12345...\n");
        client_fd = accept(network_fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;  // No client yet, try again later
            }
            fprintf(stderr, "❌ Failed to accept client connection: %s\n", strerror(errno));
            return false;
        }

        // Set client socket to non-blocking
        flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        fprintf(stdout, "✅ Client connected!\n");
        fprintf(stdout, "\n🎙️  Listening for audio... Press Ctrl+C to stop\n");
        return true;
    }
};

// First, define the WriteCallback function
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Then define the GroqProcessor class before AudioProcessor
class GroqProcessor {
private:
    std::string api_key;
    std::string model;
    CURL* curl;
    std::vector<json> conversation_history;

    void debug_response(const std::string& response) {
        fprintf(stderr, "\nAPI Response Debug:\n%s\n", response.c_str());
    }

public:
    GroqProcessor(const std::string& key, const std::string& model_name = "mixtral-8x7b-32768") 
        : api_key(key), model(model_name) {
        curl_global_init(CURL_GLOBAL_ALL);
        curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }

        // Initialize conversation with system message
        conversation_history.push_back({
            {"role", "system"},
            {"content", "You are a helpful AI assistant. Keep responses concise and relevant. "
                       "When responding to transcribed speech, ignore background noises and symbols like '*ding*' or '-'."}
        });
    }

    ~GroqProcessor() {
        if (curl) {
            curl_easy_cleanup(curl);
        }
        curl_global_cleanup();
    }

    std::string process_text(const std::string& input) {
        if (!curl) return "Error: CURL not initialized";

        // Skip processing if input contains only noise or symbols
        if (input.find_first_not_of(" -*\n.") == std::string::npos) {
            return "";  // Return empty string for noise-only input
        }

        // Add user message to history
        conversation_history.push_back({
            {"role", "user"},
            {"content", input}
        });

        // Prepare JSON payload with conversation history
        json payload = {
            {"model", model},
            {"messages", conversation_history},
            {"temperature", 0.7},
            {"max_tokens", 1000}
        };

        std::string jsonStr = payload.dump();
        std::string readBuffer;  // Declare response buffer

        // Set up CURL
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        std::string auth_header = "Authorization: Bearer " + api_key;
        headers = curl_slist_append(headers, auth_header.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, "https://api.groq.com/openai/v1/chat/completions");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

        fprintf(stdout, "\n📤 Sending request to Groq API...\n");

        // Perform request
        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);

        if (res != CURLE_OK) {
            return "Error: " + std::string(curl_easy_strerror(res));
        }

        // Parse response
        try {
            json response = json::parse(readBuffer);
            
            // Check for API errors
            if (response.contains("error")) {
                return "API Error: " + response["error"]["message"].get<std::string>();
            }

            // Validate response structure
            if (!response.contains("choices") || 
                response["choices"].empty() || 
                !response["choices"][0].contains("message") ||
                !response["choices"][0]["message"].contains("content")) {
                
                fprintf(stderr, "\n❌ Invalid API response structure:\n%s\n", readBuffer.c_str());
                return "Error: Invalid API response structure";
            }

            std::string reply = response["choices"][0]["message"]["content"].get<std::string>();
            
            // Add assistant's response to history
            conversation_history.push_back({
                {"role", "assistant"},
                {"content", reply}
            });

            // Trim history if it gets too long (keep last N messages)
            if (conversation_history.size() > 10) {
                conversation_history.erase(
                    conversation_history.begin() + 1,  // Keep system message
                    conversation_history.begin() + (conversation_history.size() - 5)
                );
            }

            return reply;

        } catch (const std::exception& e) {
            fprintf(stderr, "\n❌ Error parsing response:\n%s\n", readBuffer.c_str());
            return "Error parsing response: " + std::string(e.what());
        }
    }
};

// Then define AudioProcessor
class AudioProcessor {
private:
    whisper_context* ctx;
    AudioCapture& audio_capture;
    bool enable_timestamps;
    std::string accumulated_transcription;
    int64_t cumulative_time_ms = 0;

    static constexpr int MIN_AUDIO_LENGTH_MS = 5000;  // 5 seconds minimum
    static constexpr int SILENCE_THRESHOLD_MS = 1000;  // 1 second of silence
    static constexpr int SAMPLE_RATE = 16000;

    std::vector<float> current_buffer;
    int64_t last_voice_activity_time = 0;
    std::unique_ptr<GroqProcessor> groq_processor;

    bool detect_voice_activity(const std::vector<float>& buffer) {
        if (buffer.empty()) return false;

        float rms = 0.0f;
        float peak = 0.0f;
        
        for (float sample : buffer) {
            rms += sample * sample;
            peak = std::max(peak, std::abs(sample));
        }
        rms = std::sqrt(rms / buffer.size());

        // Print VAD values for debugging
        if (rms > VADParams::RMS_THRESHOLD * 0.5f || peak > VADParams::PEAK_THRESHOLD * 0.5f) {
            fprintf(stdout, "\rVAD - RMS: %.6f, Peak: %.6f    ", rms, peak);
            fflush(stdout);
        }

        return rms > VADParams::RMS_THRESHOLD || peak > VADParams::PEAK_THRESHOLD;
    }

    void process_speech(const std::vector<float>& buffer) {
        fprintf(stdout, "\nProcessing speech segment (%.2f seconds)...\n", 
                buffer.size() / (float)SAMPLE_RATE);
        fprintf(stdout, "🔄 Processing...");
        fflush(stdout);

        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.print_progress   = false;
        wparams.print_special    = false;
        wparams.print_timestamps = true;
        wparams.single_segment   = true;
        wparams.no_context      = true;
        wparams.language        = "en";
        wparams.suppress_blank  = true;

        if (whisper_full(ctx, wparams, buffer.data(), buffer.size()) != 0) {
            fprintf(stderr, "\r❌ Failed to process audio segment\n");
            return;
        }

        fprintf(stdout, "\r✅ Done processing    \n");

        const int n_segments = whisper_full_n_segments(ctx);
        bool got_text = false;

        for (int i = 0; i < n_segments; ++i) {
            const char* text = whisper_full_get_segment_text(ctx, i);
            if (text && strlen(text) > 0) {
                got_text = true;
                if (enable_timestamps) {
                    int64_t t0 = whisper_full_get_segment_t0(ctx, i);
                    int64_t t1 = whisper_full_get_segment_t1(ctx, i);
                    accumulated_transcription += "[" + std::to_string(t0/100) + "s -> " 
                                            + std::to_string(t1/100) + "s] " 
                                            + text + "\n";
                } else {
                    accumulated_transcription += std::string(text) + " ";
                }
            }
        }

        if (got_text) {
            output_transcription();
        }
    }

public:
    AudioProcessor(const char* model_path, AudioCapture& capture, 
                  bool timestamps, const std::string& groq_api_key = "") 
        : audio_capture(capture), enable_timestamps(timestamps) {
        
        whisper_context_params params = whisper_context_default_params();
        // Enable CUDA
        params.use_gpu = true;
        params.flash_attn = true;  // Optional: Enable flash attention for better performance
        
        ctx = whisper_init_from_file_with_params(model_path, params);
        if (!ctx) {
            throw std::runtime_error("Failed to initialize whisper context");
        }

        // Initialize Groq if API key is provided
        if (!groq_api_key.empty()) {
            groq_processor = std::make_unique<GroqProcessor>(groq_api_key);
        }
    }

    ~AudioProcessor() {
        if (ctx) {
            whisper_free(ctx);
        }
    }

    void process_audio(const std::vector<float>& audio) {
        current_buffer.insert(current_buffer.end(), audio.begin(), audio.end());

        int64_t current_time = get_current_time_ms();
        if (detect_voice_activity(audio)) {
            last_voice_activity_time = current_time;
            
            // Process when we have enough audio
            if (current_buffer.size() >= SAMPLE_RATE * MIN_AUDIO_LENGTH_MS / 1000) {
                process_speech(current_buffer);
                current_buffer.clear();
            }
        } else if (!current_buffer.empty() && 
                  current_time - last_voice_activity_time > SILENCE_THRESHOLD_MS) {
            // Process remaining audio after silence if it's long enough
            if (current_buffer.size() >= SAMPLE_RATE * 2) {  // At least 2 seconds
                process_speech(current_buffer);
            }
            current_buffer.clear();
        }
    }

    void output_transcription() {
        if (!accumulated_transcription.empty()) {
            // Skip processing if only blank audio or noise
            if (accumulated_transcription.find("[BLANK_AUDIO]") != std::string::npos ||
                accumulated_transcription.find_first_not_of(" -*\n.[],0123456789:") == std::string::npos) {
                accumulated_transcription.clear();
                return;
            }

            // Create a JSON response
            json response = {
                {"type", "transcription"},
                {"text", accumulated_transcription}
            };
            
            fprintf(stdout, "\n📝 Full Transcription:\n%s\n", accumulated_transcription.c_str());
            
            // Send transcription to WebSocket clients
            if (audio_capture.get_mode() == ConnectionMode::WEBSOCKET) {
                audio_capture.broadcast_message(response.dump());
            }
            
            if (groq_processor) {
                try {
                    fprintf(stdout, "\n🤖 Processing with Groq...\n");
                    std::string ai_response = groq_processor->process_text(accumulated_transcription);
                    
                    if (!ai_response.empty() && ai_response.find("API Error") == std::string::npos) {
                        fprintf(stdout, "\n💬 AI Response:\n%s\n", ai_response.c_str());
                        
                        // Send AI response to WebSocket clients
                        if (audio_capture.get_mode() == ConnectionMode::WEBSOCKET) {
                            json ai_msg = {
                                {"type", "ai_response"},
                                {"text", ai_response}
                            };
                            audio_capture.broadcast_message(ai_msg.dump());
                        }
                    } else if (ai_response.find("Invalid API Key") != std::string::npos) {
                        fprintf(stderr, "\n❌ Invalid Groq API key. Please check your API key and try again.\n");
                        // Disable Groq processor to prevent further API calls
                        groq_processor.reset();
                    }
                } catch (const std::exception& e) {
                    fprintf(stderr, "\n❌ Error processing with Groq: %s\n", e.what());
                }
            }
            accumulated_transcription.clear();
        }
    }
};

// Signal handler function
void signal_handler(int signal) {
    static int count = 0;
    count++;
    
    if (count == 1) {
        fprintf(stdout, "\nReceived Ctrl+C, stopping gracefully...\n");
        g_is_running = false;
    } else {
        fprintf(stdout, "\nForced exit.\n");
        exit(1);
    }
}

int main(int argc, char** argv) {
    // Add help flag check before other argument parsing
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        fprintf(stdout, R"(
Whisper LLM WebSocket Service - Real-time Speech Recognition and AI Response

Usage: %s <model_path> [options]

Required:
    <model_path>            Path to the Whisper model file (e.g., models/ggml-base.en.bin)

Options:
    --mode <mode>          Connection mode: 'websocket' or 'ssh' (default: ssh)
    --port <port>          Port number (default: 8765 for WebSocket, 12345 for SSH)
    --address <addr>       Bind address (default: 0.0.0.0)
    --timestamps           Enable timestamps in transcription
    --groq-api-key <key>   Groq API key for AI responses
    -h, --help            Show this help message

Examples:
    # Start WebSocket server with default settings
    %s models/ggml-base.en.bin --mode websocket

    # Start SSH server on custom port with Groq integration
    %s models/ggml-base.en.bin --mode ssh --port 9000 --groq-api-key YOUR_API_KEY

    # Start WebSocket server with all options
    %s models/ggml-base.en.bin --mode websocket --port 8765 --address 0.0.0.0 --timestamps --groq-api-key YOUR_API_KEY

)", argv[0], argv[0], argv[0], argv[0]);
        return 0;
    }

    // Parse command line arguments
    bool enable_timestamps = false;
    std::string groq_api_key;
    ConnectionMode mode = ConnectionMode::SSH;
    uint16_t port = 8765;  // Default WebSocket port
    std::string bind_address = "0.0.0.0";  // Default to all interfaces
    
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--timestamps") {
            enable_timestamps = true;
        }
        else if (arg == "--groq-api-key" && i + 1 < argc) {
            groq_api_key = argv[++i];
        }
        else if (arg == "--mode" && i + 1 < argc) {
            std::string mode_str = argv[++i];
            if (mode_str == "ssh") {
                mode = ConnectionMode::SSH;
                port = 12345;  // Default SSH port
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
        // Add timeout for graceful shutdown
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

        // Set up signal handler with timeout
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

        AudioProcessor processor(argv[1], *capture, enable_timestamps, groq_api_key);

        fprintf(stdout, "\n🎙️  Listening for audio... Press Ctrl+C to stop\n");

        while (g_is_running) {
            auto audio = capture->get_audio();
            if (!audio.empty()) {
                processor.process_audio(audio);
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
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
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