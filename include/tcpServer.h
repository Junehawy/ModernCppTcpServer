#pragma once
#include <functional>
#include <thread>
#include "EpollConnection.h"
#include "ClientHandler.h"
#include "SubReactor.h"
#include "socketUtils.h"

class TcpServer {
public:

    explicit TcpServer(int port = 9999, int backlog = SOMAXCONN,bool use_epoll = true);
    explicit TcpServer(int port = 9999, int backlog = SOMAXCONN,bool use_epoll = true,int num_reactors = 7);
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

    std::vector<std::unique_ptr<SubReactor>> sub_reactors_;
    std::atomic<size_t> next_reactor_index_{0};
    std::mutex connections_mutex_;
    std::vector<int> epoll_fds_;
    std::vector<std::unordered_map<int, std::shared_ptr<EpollConnection>>> connections_per_reactor_;
    std::vector<std::thread> reactor_threads_;
    int num_reactors_ = 1; //Default single reactor

    void check_idle_timeout(std::unordered_map<int,std::shared_ptr<EpollConnection>>& conns,int epfd);
    void cleanup_connections(std::unordered_map<int,std::shared_ptr<EpollConnection>>& conns,int epfd);

    void start_multi_reactor(const ClientHandler& handler);
    void distribute_connections(SocketPtr client_fd,const sockaddr_in& client_addr);
};
