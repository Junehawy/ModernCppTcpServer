#pragma once
#include "socketUtils.h"
#include "stringUtils.h"
#include <functional>
#include <memory>

class TcpServer {
public:
    // Callback type for new client connections
    using ClientHandler = std::function<void(SocketPtr client_id, const sockaddr_in& client_addr)>;

    explicit TcpServer(int port = 9999, int backlog = 1024);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void start(const ClientHandler& handler);
    void shutdown();

private:
    SocketPtr server_fd_;
    int port_;
    int backlog_;

    std::jthread accept_thread_;
    std::atomic<bool> running_ { true };
};