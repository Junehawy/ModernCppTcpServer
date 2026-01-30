#include "SubReactor.h"
#include <sys/epoll.h>
#include "EpollConnection.h"
#include "config.h"

SubReactor::SubReactor(const ClientHandler &clientHandler) :clientHandler_(clientHandler) {
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        throw std::runtime_error("epoll_create1 failed");
    }

    thread_ = std::thread(&SubReactor::run, this);
}

SubReactor::~SubReactor() {
    shutdown();
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

void SubReactor::add_connection(SocketPtr client_fd, const sockaddr_in &client_addr) {
    auto conn = clientHandler_(std::move(client_fd), client_addr);
    auto epoll_conn = std::dynamic_pointer_cast<EpollConnection>(conn);
    if (!epoll_conn) {
        throw std::runtime_error("Connection is not EpollConnection");
    }

    int fd = epoll_conn->get_fd();

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        spdlog::error("epoll_ctl add failed: {}", strerror(errno));
        return;
    }

    connections_[fd] = std::move(epoll_conn);
}

void SubReactor::run() {
    epoll_event events[EPOLL_MAX_EVENTS];

    while (running_) {
        int nfds = epoll_wait(epoll_fd_, events, EPOLL_MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            spdlog::error("epoll_wait failed: {}", strerror(errno));
            break;
        }

        for (int i=0;i<nfds;i++) {
            int fd = events[i].data.fd;

            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                auto it = connections_.find(fd);
                if (it != connections_.end()) {
                    it->second->handle_error();
                    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                    connections_.erase(it);
                }
                continue;
            }

            auto it = connections_.find(fd);
            if (it != connections_.end()) continue;

            auto& conn = it->second;

            if (events[i].events & EPOLLIN) {
                conn->handle_read();
            }
            if (events[i].events & EPOLLOUT) {
                conn->handle_write();
            }

            epoll_event ev{};
            ev.data.fd = fd;
            ev.events = EPOLLIN | EPOLLET | (conn->has_pending_write() ? EPOLLOUT : 0);
            epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
        }
        auto now = std::chrono::steady_clock::now();
        std::vector<int> to_close;
        for (const auto& pair : connections_) {
            const auto& conn = pair.second;
            if (conn->enable_idle_timeout_ && (now - conn->last_active_ > conn->idle_timeout_)) {
                to_close.push_back(pair.first);
            }
        }
        for (int fd : to_close) {
            auto it = connections_.find(fd);
            if (it != connections_.end()) {
                it->second->shutdown();
                epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                connections_.erase(it);
            }
        }
    }
}

void SubReactor::shutdown() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }

    for (auto& pair : connections_) {
        pair.second->shutdown();
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, pair.first, nullptr);
    }
    connections_.clear();
}