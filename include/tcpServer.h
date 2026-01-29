#pragma once
#include <functional>
#include <thread>
#include "EpollConnection.h"
#include "socketUtils.h"

class TcpServer {
public:
    // Callback type for new client connections
    using ClientHandler = std::function<std::shared_ptr<BaseConnection>(SocketPtr client_id, const sockaddr_in& client_addr)>;

    explicit TcpServer(int port = 9999, int backlog = SOMAXCONN,bool use_epoll = true);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void start(const ClientHandler& handler);
    void shutdown();

private:
    SocketPtr server_fd_;
    int port_;
    int backlog_;
    bool use_epoll_;

    std::jthread accept_thread_;
    std::atomic<bool> running_ { true };
    std::atomic<bool> should_stop_ { false };

    static int s_wakeup_pipe_[2];
    static std::once_flag s_pipe_init_flag;

    std::unordered_map<int, std::shared_ptr<EpollConnection>> connections_;

    void start_blocking(const ClientHandler& handler);
    void start_epoll(const ClientHandler& handler);

    static void init_wakeup_pipe();
    static void signal_handler(int sig);
};
