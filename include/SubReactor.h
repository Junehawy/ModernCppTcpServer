#pragma once
#include "ClientHandler.h"
#include "EpollConnection.h"

class SubReactor {
public:
    explicit SubReactor(const ClientHandler& clientHandler);
    ~SubReactor();

    SubReactor(const SubReactor&) = delete;
    SubReactor& operator=(const SubReactor&) = delete;
    SubReactor(SubReactor&&) = delete;
    SubReactor& operator=(SubReactor&&) = delete;

    void add_connection(net_utils::SocketPtr client_fd,const sockaddr_in& client_addr);
    void shutdown();
private:
    void run();
    void do_pending_functors();
    void check_timeouts();
    void close_connection(int fd);


    using ConnectionMap = std::unordered_map<int,std::shared_ptr<EpollConnection>>;
    using TimeoutMap = std::multimap<std::chrono::steady_clock::time_point,int>;

    int wake_fd_ = -1;
    net_utils::EpollFd epoll_fd_;

    mutable std::shared_mutex connection_mutex_;
    ConnectionMap connections_;
    TimeoutMap timeout_map_;

    ClientHandler clientHandler_;
    std::thread thread_;
    std::atomic<bool> running_{true};

    std::mutex pending_mutex_;
    std::vector<std::function<void()>> pending_functors_;
};