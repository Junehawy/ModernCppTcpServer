#pragma once
#include <netinet/tcp.h>
#include <thread>

#include "client_handler.h"
#include "common/server_metrics.h"
#include "epoll_connection.h"
#include "net_utils.h"
#include "sub_reactor.h"

// TCP server supporting multiple I/O strategies
class TcpServer {
public:
    explicit TcpServer(int port = 9999, int backlog = SOMAXCONN, size_t num_reactors = 1, size_t num_workers = 4,
                       size_t max_connections = 10000);
    ~TcpServer();

    TcpServer(const TcpServer &) = delete;
    TcpServer &operator=(const TcpServer &) = delete;

    void start(const ClientHandler &handler);
    void shutdown();
    [[nodiscard]] bool is_running() const { return running_.load(); }
    [[nodiscard]] std::stop_token get_stop_token() const { return stop_source_.get_token(); }

    void set_metrics(ServerMetrics *metrics) { metrics_ = metrics; }
    [[nodiscard]] WorkerPool *get_worker_pool() const { return worker_pool_.get(); }

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

    void start_single_epoll(const std::stop_token &st);
    void start_multi_epoll(const std::stop_token &st);

    [[nodiscard]] bool handle_accept() const;
    void wakeup() const;

    [[nodiscard]] bool check_connection_limit(int fd) const;

    std::vector<std::unique_ptr<SubReactor>> sub_reactors_;
    std::atomic<size_t> next_reactor_index_{0}; // Round-robin distribution

    std::unique_ptr<WorkerPool> worker_pool_;
    ServerMetrics *metrics_{nullptr};
    size_t max_connections_;
};
