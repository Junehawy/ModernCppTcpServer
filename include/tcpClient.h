#pragma once
#include "buffer_utils.h"
#include "message.pb.h"
#include "net_utils.h"

// Simple blocking TCP client with automatic reconnect and line buffering
class TcpClient {
public:
    explicit TcpClient(std::string server_ip = "127.0.0.1", int port = 9999);
    ~TcpClient() = default;

    TcpClient(const TcpClient &) = delete;
    TcpClient &operator=(const TcpClient &) = delete;

    // Send with automatic newline termination
    void send_message(const std::string &msg);

    // Blocking receive until newline or error
    std::string receive_line();
    std::string receive(size_t max_len);

    bool is_connected() const { return connected_.load(std::memory_order_acquire); }

    void send_protobuf(const moderncpp::Request& req);
    moderncpp::Response receive_protobuf();
private:
    net_utils::SocketPtr sock_fd_;
    std::string server_ip_;
    int port_;
    std::atomic<bool> connected_{false};

    Buffer recv_buf_;   // Read buffer for line reconstruction
    std::chrono::steady_clock::time_point last_active_time_;

    void ensure_connection();
};
