#include "SubReactor.h"

#include <ranges>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include "EpollConnection.h"
#include "config.h"

SubReactor::SubReactor(const ClientHandler &clientHandler) : clientHandler_(clientHandler) {
    epoll_fd_ = net_utils::EpollFd();

    // Create eventfd for cross-thread wakeup
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        throw std::runtime_error("eventfd creation failed");
    }
    net_utils::epoll_add(epoll_fd_, wake_fd_, EPOLLIN);

    thread_ = std::jthread([this](const std::stop_token &st){run(st);});
}

SubReactor::~SubReactor() { shutdown(); }

// Thread-safe: called from acceptor thread
void SubReactor::add_connection(net_utils::SocketPtr client_fd, const sockaddr_in &client_addr) {
    const auto conn = clientHandler_(std::move(client_fd), client_addr);
    auto epoll_conn = std::dynamic_pointer_cast<EpollConnection>(conn);
    if (!epoll_conn) {
        throw std::runtime_error("Connection is not EpollConnection");
    }

    int fd = epoll_conn->get_fd();
    net_utils::set_nonblocking(fd);

    constexpr int flag = 1;
    if (setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&flag,sizeof(flag)) < 0) {
        NET_LOG_ERROR("Failed to set TCP_NODELAY");
    }

    epoll_conn->attch_reactor(this);

    // Queue functor to execute in reactor thread
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

    constexpr uint64_t one = 1;
    ::write(wake_fd_, &one, sizeof(one));   // Wake up epoll_wait
}

void SubReactor::run(const std::stop_token &st) {
    epoll_event events[EPOLL_MAX_EVENTS];

    while (!st.stop_requested()) {
        const int nfds = epoll_wait(epoll_fd_, events, EPOLL_MAX_EVENTS, EPOLL_WAIT_TIMEOUT.count());
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

            // Handle events
            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                conn->handle_error();
                close_connection(fd);
            } else {
                if (events[i].events & EPOLLIN) {
                    conn->handle_read();
                }
                if (events[i].events & EPOLLOUT && conn->is_alive()) {
                    conn->handle_write();
                }

                if (!conn->is_alive()) {
                    close_connection(fd);
                }
            }
        }

        check_timeouts();
        do_pending_functors();
    }

    // Cleanup on exit
    std::vector<int> fds;
    {
        std::unique_lock lock(connection_mutex_);
        for (const auto &fd: connections_ | std::views::keys) fds.emplace_back(fd);
    }
    for (const int fd : fds) close_connection(fd);
}

// Called by connection when output buffer becomes non-empty
void SubReactor::enable_writing(int fd) {
    {
        std::lock_guard lock(pending_mutex_);
        pending_functors_.emplace_back([this,fd]() {
            std::shared_lock conn_lock(connection_mutex_);
            if (!connections_.contains(fd)) return;

            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLET | EPOLLOUT;
            ev.data.fd = fd;
            if (epoll_ctl(epoll_fd_.get(),EPOLL_CTL_MOD,fd,&ev) < 0) {
                if (errno != ENOENT && errno != EBADF) {
                    NET_LOG_WARN("Epoll_mod enable_write failed: {}",strerror(errno));
                }
            }
        });
    }
    constexpr uint64_t one = 1;
    ::write(wake_fd_,&one,sizeof(one));
}

void SubReactor::disable_writing(int fd) {
    {
        std::lock_guard lock(pending_mutex_);
        pending_functors_.emplace_back([this,fd]() {
            std::shared_lock conn_lock(connection_mutex_);
            if (!connections_.contains(fd)) return;

            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLET;      //Remove EPOLLOUT
            ev.data.fd = fd;
            if (epoll_ctl(epoll_fd_.get(),EPOLL_CTL_MOD,fd,&ev) < 0) {
                if (errno != ENOENT && errno != EBADF) {
                    NET_LOG_WARN("Epoll_mod disable_write failed: {}",strerror(errno));
                }
            }
        });
    }
}

void SubReactor::close_connection(const int fd) {
    std::shared_ptr<EpollConnection> conn;

    {
        std::unique_lock lock(connection_mutex_);
        const auto it = connections_.find(fd);
        if (it == connections_.end()) {
            return;
        }

        conn = std::move(it->second);
        connections_.erase(it);

        for (auto pair = timeout_map_.begin(); pair != timeout_map_.end();) {
            if (pair->second == fd) {
                pair = timeout_map_.erase(pair);
            } else {
                ++pair;
            }
        }
    }

    if (conn) {
        net_utils::epoll_del(epoll_fd_.get(), fd);

        conn->shutdown();
    }
}

// Close connections idle longer than configured timeout
void SubReactor::check_timeouts() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<int> timeouts_fds;

    {
        std::unique_lock lock(connection_mutex_);
        auto it = timeout_map_.begin();
        while (it != timeout_map_.end() && it->first <= now) {
            int fd = it->second;
            if (auto conn_it = connections_.find(fd);
                conn_it != connections_.end() && conn_it->second->is_idle_timeout_enabled()) {
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
            if (auto it = connections_.find(fd); it != connections_.end())
                conn = it->second;
        }
        if (conn) {
            NET_LOG_WARN("Idle timeout for {}, closing", conn->get_peer_info());
            conn->shutdown();
        }
    }
}

// Execute queued functors
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
    if (bool expected = false; !shutdown_done_.compare_exchange_strong(expected, true)) return;

    if (wake_fd_ != -1) {
        constexpr uint64_t one = 1;
        while (::write(wake_fd_, &one, sizeof(one)) < 0 && errno == EINTR);
    }

    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }

    {
        std::unique_lock lock(connection_mutex_);
        for (auto &[fd,conn]: connections_) {
            net_utils::epoll_del(epoll_fd_.get(), fd);
            conn->shutdown();
        }
        connections_.clear();
        timeout_map_.clear();
    }

    net_utils::close_safe(wake_fd_);
    wake_fd_ = -1;
}
