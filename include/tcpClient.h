#pragma once
#include "socketUtils.h"

class TcpClient {
public:
    TcpClient(const std::string& server_ip = "127.0.0.1", int port = 9999);
    ~TcpClient() = default;

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    void send_message(const std::string& msg);
    std::string receive_line();

private:
    SocketPtr sock_fd_;
};