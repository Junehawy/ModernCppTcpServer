#pragma once
#include <netinet/tcp.h>
#include <thread>

#include "ClientHandler.h"
#include "EpollConnection.h"
#include "SubReactor.h"
#include "net_utils.h"


// TCP server supporting multiple I/O strategies
class TcpServer {
public:
    explicit TcpServer(int port = 9999, int backlog = SOMAXCONN, bool use_epoll = true);
    explicit TcpServer(int port = 9999, int backlog = SOMAXCONN, bool use_epoll = true, int num_reactors = 1);
    ~TcpServer();

    TcpServer(const TcpServer &) = delete;
    TcpServer &operator=(const TcpServer &) = delete;

    void start(const ClientHandler &handler);
    void shutdown();
    bool is_running() const { return !stop_source_.stop_requested(); }

    static TcpServer *instance() { return instance_.load(std::memory_order_acquire); }

private:
    net_utils::SocketPtr server_fd_;
    int port_;
    int backlog_;
    bool use_epoll_;
    int num_reactors_ = 1;  // Default single reactor

    std::jthread accept_thread_;
    std::stop_source stop_source_;

    ClientHandler client_handler_;

    static std::atomic<TcpServer *> instance_;
    static std::mutex instance_mutex_;

    int wakeup_pipe_[2] = {-1, -1};

    void start_blocking(const ClientHandler &handler, const std::stop_token &st) const;
    void start_single_epoll(const ClientHandler &handler, const std::stop_token &st);
    void start_multi_epoll(const ClientHandler &handler, const std::stop_token &st);

    void handle_accept(int listen_fd, const ClientHandler &handler) const;
    void wakeup() const;

    std::vector<std::unique_ptr<SubReactor>> sub_reactors_;
    std::atomic<size_t> next_reactor_index_{0};     // Round-robin distribution

    static void signal_handler(int sig);
};
