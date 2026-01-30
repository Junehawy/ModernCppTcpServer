#pragma once
#include "ClientHandler.h"
#include "EpollConnection.h"

class SubReactor {
public:
    SubReactor(const ClientHandler& clientHandler);
    ~SubReactor();

    SubReactor(const SubReactor&) = delete;
    SubReactor& operator=(const SubReactor&) = delete;
    SubReactor(SubReactor&&) = delete;
    SubReactor& operator=(SubReactor&&) = delete;

    void add_connection(SocketPtr client_fd,const sockaddr_in& client_addr);
    void shutdown();
private:
    void run();

    int epoll_fd_ = -1;
    std::unordered_map<int,std::shared_ptr<EpollConnection>> connections_;
    ClientHandler clientHandler_;

    std::thread thread_;
    std::atomic<bool> running_{true};
};