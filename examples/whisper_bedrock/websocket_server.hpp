#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <functional>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <atomic>
#include <queue>
#include <nlohmann/json.hpp>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;

class WebSocketServer; // Forward declaration of WebSocketServer

// Now fully define WebSocketSession
class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
private:
    websocket::stream<tcp::socket> ws;
    WebSocketServer& server;
    std::string id;
    beast::flat_buffer buffer;
    
public:
    WebSocketSession(tcp::socket socket, WebSocketServer& server_ref);
    
    void start();
    void send(const std::string& message);
    void close();
    
private:
    void do_read();
};

// Now define WebSocketServer
class WebSocketServer {
private:
    struct ConnectionState {
        std::string id;
        int64_t last_ping_time = 0;
        int64_t last_message_time = 0;
        std::queue<std::vector<float>> audio_queue;
        std::mutex queue_mutex;
        std::string contact;
        std::shared_ptr<WebSocketSession> session;
    };

    net::io_context ioc;
    tcp::acceptor acceptor;
    std::thread server_thread;
    std::atomic<bool> running{false};
    std::mutex clients_mutex;
    std::unordered_map<std::string, std::shared_ptr<ConnectionState>> clients;
    
    // Callbacks
    std::function<void(std::string, std::vector<float>)> audio_callback;
    std::function<void(std::string, std::string)> contact_callback;

    int port = 8765;

public:
    WebSocketServer() : ioc(1), acceptor(ioc) {}
    
    ~WebSocketServer() {
        stop();
    }
    
    void start() {
        if (running) return;
        
        try {
            tcp::endpoint endpoint(tcp::v4(), port);
            acceptor.open(endpoint.protocol());
            acceptor.set_option(net::socket_base::reuse_address(true));
            acceptor.bind(endpoint);
            acceptor.listen(net::socket_base::max_listen_connections);
            
            running = true;
            
            // Start accepting connections in a separate thread
            server_thread = std::thread([this]() {
                try {
                    do_accept();
                    ioc.run();
                } catch (std::exception& e) {
                    fprintf(stderr, "Server error: %s\n", e.what());
                    running = false;
                }
            });
            
            fprintf(stdout, "WebSocket server started on port %d\n", port);
        } catch (std::exception& e) {
            fprintf(stderr, "Failed to start server: %s\n", e.what());
            running = false;
        }
    }
    
    void stop() {
        if (!running) return;
        
        // First, set running flag to false to prevent new operations
        running = false;
        
        try {
            // Log the shutdown process
            fprintf(stdout, "WebSocket server stopping: closing acceptor...\n");
            
            // Cancel the acceptor first (with timeout protection)
            beast::error_code ec;
            acceptor.cancel(ec);
            if (ec) {
                fprintf(stderr, "Error canceling acceptor: %s\n", ec.message().c_str());
            }
            
            // Close all client connections with timeout
            {
                fprintf(stdout, "WebSocket server stopping: closing client connections...\n");
                std::lock_guard<std::mutex> lock(clients_mutex);
                
                // Make a copy of clients to avoid iterator invalidation
                auto client_copies = clients;
                
                // Close each client connection
                for (auto& client : client_copies) {
                    if (client.second && client.second->session) {
                        try {
                            // Use a non-blocking approach to close each connection
                            auto session = client.second->session;
                            session->close();
                        } catch (std::exception& e) {
                            fprintf(stderr, "Error closing client %s: %s\n", 
                                    client.first.c_str(), e.what());
                        }
                    }
                }
                
                // Clear the client map
                clients.clear();
            }
            
            // Stop the IO context and join the server thread
            fprintf(stdout, "WebSocket server stopping: stopping IO context...\n");
            ioc.stop();
            
            if (server_thread.joinable()) {
                // Use a timeout for joining the thread
                auto join_thread = std::thread([this]() {
                    server_thread.join();
                });
                
                // Wait with timeout
                if (join_thread.joinable()) {
                    join_thread.detach();
                }
            }
            
            fprintf(stdout, "WebSocket server stopped\n");
        } catch (std::exception& e) {
            fprintf(stderr, "Error stopping server: %s\n", e.what());
        }
    }
    
    void send_to_all(const json& message) {
        std::string text = message.dump();
        std::lock_guard<std::mutex> lock(clients_mutex);
        
        for (auto& client : clients) {
            send_to_client(client.first, text);
        }
    }
    
    void send_to_client(const std::string& client_id, const std::string& message) {
        std::lock_guard<std::mutex> lock(clients_mutex);
        
        auto it = clients.find(client_id);
        if (it != clients.end() && it->second->session) {
            try {
                it->second->session->send(message);
            } catch (std::exception& e) {
                fprintf(stderr, "Error sending to client %s: %s\n", client_id.c_str(), e.what());
            }
        }
    }
    
    void on_audio(std::function<void(std::string, std::vector<float>)> callback) {
        audio_callback = std::move(callback);
    }
    
    void on_contact(std::function<void(std::string, std::string)> callback) {
        contact_callback = std::move(callback);
    }
    
    void add_audio(const std::string& client_id, const std::vector<float>& audio) {
        std::lock_guard<std::mutex> lock(clients_mutex);
        
        auto it = clients.find(client_id);
        if (it != clients.end()) {
            std::lock_guard<std::mutex> queue_lock(it->second->queue_mutex);
            it->second->audio_queue.push(audio);
        }
    }
    
    bool is_running() const {
        return running;
    }

    void set_audio_callback(std::function<void(const std::vector<float>&)> callback) {
        audio_callback = [callback](std::string client_id, std::vector<float> audio) {
            callback(audio);
        };
    }
    
    void set_contact_update_callback(std::function<void(const std::string&)> callback) {
        contact_callback = [callback](std::string client_id, std::string contact) {
            callback(contact);
        };
    }
    
    void broadcast_text(const std::string& message) {
        json j = json::parse(message);
        send_to_all(j);
    }
    
    bool needs_restart() const {
        return false; // Implement real logic if needed
    }

    WebSocketServer(const char* address, int port) : ioc(1), acceptor(ioc) {
        this->port = port;
    }

private:
    void do_accept();
    
    std::string generate_id() {
        static std::atomic<uint64_t> counter{0};
        return std::to_string(counter++);
    }
    
    int64_t get_current_time_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
    
    void handle_message(const std::string& client_id, const std::string& payload) {
        // Check if the message is likely binary audio data
        if (payload.size() >= 4 && !is_valid_json(payload)) {
            // Process as binary audio data
            process_audio_data(client_id, payload);
        } else {
            // Process as JSON message
            try {
                json data = json::parse(payload);
                
                if (data.contains("type")) {
                    std::string type = data["type"];
                    
                    if (type == "ping") {
                        std::lock_guard<std::mutex> lock(clients_mutex);
                        auto it = clients.find(client_id);
                        if (it != clients.end()) {
                            it->second->last_ping_time = get_current_time_ms();
                        }
                        
                        // Send pong
                        json pong = {
                            {"type", "pong"},
                            {"time", get_current_time_ms()}
                        };
                        send_to_client(client_id, pong.dump());
                    }
                    else if (type == "contact" && data.contains("contact_id")) {
                        std::string contact_id = data["contact_id"];
                        
                        {
                            std::lock_guard<std::mutex> lock(clients_mutex);
                            auto it = clients.find(client_id);
                            if (it != clients.end()) {
                                it->second->contact = contact_id;
                            }
                        }
                        
                        if (contact_callback) {
                            contact_callback(client_id, contact_id);
                        }
                    }
                }
            } catch (const json::exception& e) {
                fprintf(stderr, "Error parsing message: %s\n", e.what());
            }
        }
    }
    
    bool is_valid_json(const std::string& str) {
        if (str.empty()) return false;
        char first_char = str.front();
        return first_char == '{' || first_char == '[' || first_char == '"' || 
               (first_char >= '0' && first_char <= '9') || 
               first_char == 't' || first_char == 'f' || first_char == 'n';
    }
    
    void process_audio_data(const std::string& client_id, const std::string& binary_data) {
        // Convert binary data to float vector
        const float* float_data = reinterpret_cast<const float*>(binary_data.data());
        size_t num_samples = binary_data.size() / sizeof(float);
        
        std::vector<float> audio_data(float_data, float_data + num_samples);
        
        // Now call the audio callback
        if (audio_callback) {
            audio_callback(client_id, audio_data);
        }
    }
    
    void check_connections() {
        std::lock_guard<std::mutex> lock(clients_mutex);
        int64_t current_time = get_current_time_ms();
        
        // Check for inactive connections
        for (auto it = clients.begin(); it != clients.end();) {
            if (current_time - it->second->last_message_time > 60000) { // 60 seconds timeout
                fprintf(stdout, "⚠️ Removing inactive connection: %s\n", it->first.c_str());
                it = clients.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    // New method to handle just JSON messages
    void handle_json_message(const std::string& client_id, const std::string& payload) {
        try {
            json data = json::parse(payload);
            
            if (data.contains("type")) {
                std::string type = data["type"];
                
                if (type == "ping") {
                    std::lock_guard<std::mutex> lock(clients_mutex);
                    auto it = clients.find(client_id);
                    if (it != clients.end()) {
                        it->second->last_ping_time = get_current_time_ms();
                    }
                    
                    // Send pong
                    json pong = {
                        {"type", "pong"},
                        {"time", get_current_time_ms()}
                    };
                    send_to_client(client_id, pong.dump());
                }
                else if (type == "contact" && data.contains("contact_id")) {
                    std::string contact_id = data["contact_id"];
                    
                    {
                        std::lock_guard<std::mutex> lock(clients_mutex);
                        auto it = clients.find(client_id);
                        if (it != clients.end()) {
                            it->second->contact = contact_id;
                        }
                    }
                    
                    if (contact_callback) {
                        contact_callback(client_id, contact_id);
                    }
                }
            }
        } catch (const json::exception& e) {
            fprintf(stderr, "Error parsing JSON message: %s\n", e.what());
        }
    }
    
    friend class WebSocketSession;
};

// Implementation of do_accept
inline void WebSocketServer::do_accept() {
    acceptor.async_accept(
        [this](beast::error_code ec, tcp::socket socket) {
            if(ec) {
                fprintf(stderr, "Accept error: %s\n", ec.message().c_str());
            } else {
                std::make_shared<WebSocketSession>(std::move(socket), *this)->start();
            }
            
            // Continue accepting if server is still running
            if(running) {
                do_accept();
            }
        });
}

// WebSocketSession constructor implementation
inline WebSocketSession::WebSocketSession(tcp::socket socket, WebSocketServer& server_ref)
    : ws(std::move(socket)), server(server_ref) {
    id = server.generate_id();
}

// WebSocketSession start method implementation
inline void WebSocketSession::start() {
    // Accept the WebSocket handshake
    ws.async_accept(
        [self = shared_from_this()](beast::error_code ec) {
            if(ec) {
                fprintf(stderr, "Error accepting WebSocket: %s\n", ec.message().c_str());
                return;
            }
            
            // Create client state
            std::shared_ptr<WebSocketServer::ConnectionState> state = 
                std::make_shared<WebSocketServer::ConnectionState>();
            state->id = self->id;
            state->last_message_time = self->server.get_current_time_ms();
            state->session = self;
            
            {
                std::lock_guard<std::mutex> lock(self->server.clients_mutex);
                self->server.clients[self->id] = state;
            }
            
            fprintf(stdout, "New WebSocket connection: %s\n", self->id.c_str());
            
            // Start reading messages
            self->do_read();
        });
}

// WebSocketSession send method implementation
inline void WebSocketSession::send(const std::string& message) {
    ws.async_write(
        net::buffer(message),
        [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
            if(ec) {
                fprintf(stderr, "Error writing to WebSocket: %s\n", ec.message().c_str());
            }
        });
}

// WebSocketSession do_read method implementation
inline void WebSocketSession::do_read() {
    ws.async_read(
        buffer,
        [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
            if(ec) {
                // Connection closed or error
                if(ec != websocket::error::closed) {
                    fprintf(stderr, "Error reading from WebSocket: %s\n", ec.message().c_str());
                }
                
                // Remove client
                std::lock_guard<std::mutex> lock(self->server.clients_mutex);
                self->server.clients.erase(self->id);
                return;
            }
            
            // Update last message time
            {
                std::lock_guard<std::mutex> lock(self->server.clients_mutex);
                auto it = self->server.clients.find(self->id);
                if (it != self->server.clients.end()) {
                    it->second->last_message_time = self->server.get_current_time_ms();
                }
            }
            
            // Get the message
            std::string message = beast::buffers_to_string(self->buffer.data());
            self->buffer.consume(self->buffer.size());
            
            // Check if this is a binary or text message
            if (self->ws.got_binary()) {
                // Process as binary data
                self->server.process_audio_data(self->id, message);
            } else {
                // Process as text (JSON) message
                self->server.handle_json_message(self->id, message);
            }
            
            // Continue reading
            self->do_read();
        });
}

// WebSocketSession close method implementation
inline void WebSocketSession::close() {
    beast::error_code ec;
    ws.close(websocket::close_code::normal, ec);
    if (ec) {
        fprintf(stderr, "Error closing WebSocket session: %s\n", ec.message().c_str());
    }
} 