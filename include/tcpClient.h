#pragma once
#include "buffer_utils.h"
#include "net_utils.h"

class TcpClient {
public:
    TcpClient(const std::string& server_ip = "127.0.0.1", int port = 9999);
    ~TcpClient() = default;

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    void send_message(const std::string& msg);
    std::string receive_line();
    std::string receive(size_t max_len);

    bool is_connected() const {return connected_.load(std::memory_order_acquire);}

private:
    net_utils::SocketPtr sock_fd_;
    std::string server_ip_;
    int port_;
    std::atomic<bool> connected_{false};

    Buffer recv_buf_;
    std::chrono::steady_clock::time_point last_active_time_;

    void ensure_connection();
};