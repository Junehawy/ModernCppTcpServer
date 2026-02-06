#pragma once
#include <netinet/tcp.h>
#include <signal.h>
#include <thread>

#include "ClientHandler.h"
#include "EpollConnection.h"
#include "SubReactor.h"
#include "net_utils.h"

// TCP server supporting multiple I/O strategies
class TcpServer {
public:
    explicit TcpServer(int port = 9999, int backlog = SOMAXCONN, size_t num_reactors = 1);
    ~TcpServer();

    TcpServer(const TcpServer &) = delete;
    TcpServer &operator=(const TcpServer &) = delete;

    void start(const ClientHandler &handler);
    void shutdown();
    [[nodiscard]] bool is_running() const { return running_.load(); }
    std::stop_token get_stop_token() const {return stop_source_.get_token();}

private:
    net_utils::SocketPtr server_fd_;
    int port_;
    int backlog_;
    size_t num_reactors_;

    std::jthread accept_thread_;
    std::atomic<bool> running_{false};
    std::stop_source stop_source_;

    ClientHandler client_handler_;

    int event_fd_ = -1;

    void start_blocking(std::stop_token st) const;
    void start_single_epoll(std::stop_token st);
    void start_multi_epoll(std::stop_token st);

    bool handle_accept() const;
    void wakeup() const;


    std::vector<std::unique_ptr<SubReactor>> sub_reactors_;
    std::atomic<size_t> next_reactor_index_{0};     // Round-robin distribution
};