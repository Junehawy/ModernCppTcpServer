#include "SubReactor.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include "EpollConnection.h"
#include "config.h"

SubReactor::SubReactor(const ClientHandler &clientHandler) : clientHandler_(clientHandler) {
    epoll_fd_ = net_utils::EpollFd();

    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        throw std::runtime_error("eventfd creation failed");
    }
    net_utils::epoll_add(epoll_fd_, wake_fd_, EPOLLIN);

    thread_ = std::thread(&SubReactor::run, this);
}

SubReactor::~SubReactor() { shutdown(); }

void SubReactor::add_connection(net_utils::SocketPtr client_fd, const sockaddr_in &client_addr) {
    auto conn = clientHandler_(std::move(client_fd), client_addr);
    auto epoll_conn = std::dynamic_pointer_cast<EpollConnection>(conn);
    if (!epoll_conn) {
        throw std::runtime_error("Connection is not EpollConnection");
    }

    int fd = epoll_conn->get_fd();
    net_utils::set_nonblocking(fd);

    int flag = 1;
    if (setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&flag,sizeof(flag)) < 0) {
        NET_LOG_ERROR("Failed to set TCP_NODELAY");
    }

    {
        std::lock_guard lock(pending_mutex_);
        pending_functors_.emplace_back([this, fd, epoll_conn = std::move(epoll_conn)]() mutable {
            net_utils::epoll_add(epoll_fd_.get(), fd, EPOLLIN | EPOLLET);

            std::unique_lock conn_lock(connection_mutex_);
            connections_[fd] = std::move(epoll_conn);

            auto timeout_at = std::chrono::steady_clock::now() + connections_[fd]->get_idle_timeout();
            timeout_map_.insert({timeout_at, fd});
        });
    }

    uint64_t one = 1;
    ::write(wake_fd_, &one, sizeof(one));
}

void SubReactor::run() {
    epoll_event events[EPOLL_MAX_EVENTS];

    while (running_) {
        int nfds = epoll_wait(epoll_fd_, events, EPOLL_MAX_EVENTS, EPOLL_WAIT_TIMEOUT.count());
        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            NET_LOG_ERROR("epoll_wait failed: {}", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == wake_fd_) {
                uint64_t dummy;
                while (::read(wake_fd_, &dummy, sizeof(dummy)) > 0) {
                }
                do_pending_functors();
                continue;
            }

            std::shared_ptr<EpollConnection> conn;
            {
                std::shared_lock lock(connection_mutex_);
                auto it = connections_.find(fd);
                if (it == connections_.end())
                    continue;
                conn = it->second;
            }

            bool need_close = false;

            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                conn->handle_error();
                need_close = true;
            } else {
                if (events[i].events & EPOLLIN) {
                    conn->handle_read();
                }
                if (events[i].events & EPOLLOUT) {
                    conn->handle_write();
                }

                if (!conn->is_alive()) {
                    need_close = true;
                }
            }

            if (need_close) {
                close_connection(fd);
                continue;
            }
            uint32_t new_ev = EPOLLIN | EPOLLET;
            if (conn->has_pending_write()) {
                new_ev |= EPOLLOUT;
            }

            epoll_event ev{};
            ev.events = new_ev;
            ev.data.fd = fd;
            if (epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, fd, &ev) < 0) {
                if (errno == ENOENT) {
                    close_connection(fd);
                } else if (errno != EBADF) {
                    NET_LOG_WARN("epoll_mod failed for fd {}: {}", fd, strerror(errno));
                }
            }
        }

        check_timeouts();
        do_pending_functors();
    }
}

void SubReactor::close_connection(int fd) {
    std::shared_ptr<EpollConnection> conn;

    {
        std::unique_lock lock(connection_mutex_);
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
            return;
        }

        conn = std::move(it->second);
        connections_.erase(it);

        for (auto it = timeout_map_.begin(); it != timeout_map_.end();) {
            if (it->second == fd) {
                it = timeout_map_.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (conn) {
        net_utils::epoll_del(epoll_fd_.get(), fd);

        conn->shutdown();
    }
}

void SubReactor::check_timeouts() {
    auto now = std::chrono::steady_clock::now();
    std::vector<int> timeouts_fds;

    {
        std::unique_lock lock(connection_mutex_);
        auto it = timeout_map_.begin();
        while (it != timeout_map_.end() && it->first <= now) {
            int fd = it->second;
            auto conn_it = connections_.find(fd);
            if (conn_it != connections_.end() && conn_it->second->is_idle_timeout_enabled()) {
                if (now - conn_it->second->get_last_active() > conn_it->second->get_idle_timeout()) {
                    timeouts_fds.emplace_back(fd);
                }
            }
            it = timeout_map_.erase(it);
        }
    }

    for (int fd: timeouts_fds) {
        std::shared_ptr<EpollConnection> conn;
        {
            std::shared_lock lock(connection_mutex_);
            auto it = connections_.find(fd);
            if (it != connections_.end())
                conn = it->second;
        }
        if (conn) {
            NET_LOG_WARN("Idle timeout for {}, closing", conn->get_peer_info());
            conn->shutdown();
        }
    }
}

void SubReactor::do_pending_functors() {
    std::vector<std::function<void()>> functors;
    {
        std::lock_guard lock(pending_mutex_);
        functors.swap(pending_functors_);
    }
    for (auto &f: functors) {
        f();
    }
}

void SubReactor::shutdown() {
    if (!running_.exchange(false))
        return;

    if (wake_fd_ != -1) {
        uint64_t one = 1;
        ::write(wake_fd_, &one, sizeof(one));
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    {
        std::unique_lock lock(connection_mutex_);
        for (auto &[fd, conn]: connections_) {
            conn->shutdown();
            net_utils::epoll_del(epoll_fd_.get(), fd, std::source_location::current());
        }
        connections_.clear();
        timeout_map_.clear();
    }

    net_utils::close_safe(wake_fd_);
    wake_fd_ = -1;
}
