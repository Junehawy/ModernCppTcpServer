#pragma once
#include "ClientHandler.h"
#include "EpollConnection.h"

// Worker thread managing a subset of connections
class SubReactor {
public:
    explicit SubReactor(const ClientHandler& clientHandler);
    ~SubReactor();

    SubReactor(const SubReactor&) = delete;
    SubReactor& operator=(const SubReactor&) = delete;
    SubReactor(SubReactor&&) = delete;
    SubReactor& operator=(SubReactor&&) = delete;

    // Thread-safe addition from acceptor thread
    void add_connection(net_utils::SocketPtr client_fd,const sockaddr_in& client_addr);
    void shutdown();

    // Called by connections to enable/disable write watching
    void enable_writing(int fd);
    void disable_writing(int fd);
private:
    void run(const std::stop_token &st);   // Main epoll loop
    void do_pending_functors();     // Execute queued lambdas
    void check_timeouts();          // Close idle connections
    void close_connection(int fd);  //Clean up specific connection

    using ConnectionMap = std::unordered_map<int,std::shared_ptr<EpollConnection>>;
    using TimeoutMap = std::multimap<std::chrono::steady_clock::time_point,int>;

    int wake_fd_ = -1;              // eventfd for inter-thread wakeup
    net_utils::EpollFd epoll_fd_;
    ClientHandler clientHandler_;

    mutable std::shared_mutex connection_mutex_;
    ConnectionMap connections_;
    TimeoutMap timeout_map_;


    std::jthread thread_;
    std::atomic<bool> shutdown_done_{false};

    std::mutex pending_mutex_;
    std::vector<std::function<void()>> pending_functors_;
};