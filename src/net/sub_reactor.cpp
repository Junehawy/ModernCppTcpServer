#include "../../include/net/sub_reactor.h"

#include <ranges>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include "../../include/common/config.h"
#include "../../include/net/epoll_connection.h"

SubReactor::SubReactor(ClientHandler clientHandler) : clientHandler_(std::move(clientHandler)) {
    try {
        epoll_fd_ = net_utils::EpollFd();

        // Create eventfd for cross-thread wakeup
        wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd_ < 0) {
            throw std::runtime_error("eventfd creation failed");
        }
        net_utils::epoll_add(epoll_fd_, wake_fd_, EPOLLIN);

        timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (timer_fd_ < 0) {
            throw std::runtime_error("timerfd creation failed");
        }

        itimerspec ts{};
        ts.it_value.tv_sec = 5; // first 5s
        ts.it_interval.tv_sec = 5; // pre 5s
        timerfd_settime(timer_fd_, 0, &ts, nullptr);

        net_utils::epoll_add(epoll_fd_, timer_fd_, EPOLLIN);

        thread_ = std::jthread([this](const std::stop_token &st) { run(st); });
    } catch (...) {
        // Cleanup on construction failure
        if (wake_fd_ >= 0) {
            net_utils::close_safe(wake_fd_);
            wake_fd_ = -1;
        }

        if (timer_fd_ >= 0) {
            net_utils::close_safe(timer_fd_);
            timer_fd_ = -1;
        }
    }
}

SubReactor::~SubReactor() { shutdown(); }

// Thread-safe: called from acceptor thread
void SubReactor::add_connection(net_utils::SocketPtr client_fd, const sockaddr_in &client_addr) {
    if (!client_fd || !client_fd->valid()) {
        NET_LOG_ERROR("Invalid socket passed to add_connection");
        return;
    }

    try {
        const auto conn = clientHandler_(std::move(client_fd), client_addr);
        if (!conn) {
            NET_LOG_ERROR("Client handler returned null connection");
            return;
        }

        auto epoll_conn = std::dynamic_pointer_cast<EpollConnection>(conn);
        if (!epoll_conn) {
            NET_LOG_ERROR("Connection is not EpollConnection");
            return;
        }

        int fd = epoll_conn->get_fd();
        if (fd < 0) {
            NET_LOG_ERROR("Invalid file descriptor from connection");
            return;
        }

        net_utils::set_nonblocking(fd);

        constexpr int flag = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
            NET_LOG_ERROR("Failed to set TCP_NODELAY");
        }

        epoll_conn->attch_reactor(this);

        // Queue functor to execute in reactor thread
        {
            std::lock_guard lock(pending_mutex_);
            pending_functors_.emplace_back([this, fd, epoll_conn = std::move(epoll_conn)]() mutable {
                try {
                    if (fd < 0 || !epoll_conn || !epoll_conn->is_alive()) {
                        NET_LOG_WARN("Connection became invalid before adding to epoll");
                        return;
                    }
                    net_utils::epoll_add(epoll_fd_.get(), fd, EPOLLIN | EPOLLET);

                    if (connections_.contains(fd)) {
                        NET_LOG_WARN("FD {} already exists in connections map", fd);
                        return;
                    }

                    connections_[fd] = std::move(epoll_conn);

                    auto timeout_at = std::chrono::steady_clock::now() + connections_[fd]->get_idle_timeout();
                    timeout_map_.insert({timeout_at, fd});
                } catch (const std::exception &e) {
                    NET_LOG_ERROR("Failed to add connection to reactor: {}", e.what());
                    if (epoll_conn) {
                        epoll_conn->shutdown();
                    }
                }
            });
        }

        constexpr uint64_t one = 1;
        ::write(wake_fd_, &one, sizeof(one)); // Wake up epoll_wait
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Exception in add_connection: {}", e.what());
    }
}

void SubReactor::run(const std::stop_token &st) {
    epoll_event events[EPOLL_MAX_EVENTS];

    try {
        while (!st.stop_requested()) {
            const int nfds = epoll_wait(epoll_fd_, events, EPOLL_MAX_EVENTS, -1);
            if (nfds < 0) {
                if (errno == EINTR)
                    continue;
                NET_LOG_ERROR("epoll_wait failed: {}", strerror(errno));
                break;
            }

            bool need_pending = false;

            for (int i = 0; i < nfds; i++) {
                int fd = events[i].data.fd;

                if (fd == wake_fd_) {
                    uint64_t dummy;
                    while (::read(wake_fd_, &dummy, sizeof(dummy)) > 0) {
                    }
                    need_pending = true;
                    continue;
                }

                if (fd == timer_fd_) {
                    uint64_t expirations;
                    ::read(timer_fd_, &expirations, sizeof(expirations));
                    check_timeouts();
                    continue;
                }

                auto it = connections_.find(fd);
                if (it == connections_.end())
                    continue;
                const std::shared_ptr<EpollConnection> conn = it->second;


                if (!conn || !conn->is_alive()) {
                    close_connection(fd);
                    continue;
                }

                try {
                    // Handle events
                    if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                        conn->handle_error();
                        close_connection(fd);
                    } else {
                        if (events[i].events & EPOLLIN) {
                            try {
                                conn->handle_read();
                            } catch (const std::exception &e) {
                                NET_LOG_ERROR("Exception in handle_read for fd {}: {}", fd, e.what());
                                conn->shutdown();
                            }
                        }
                        if (events[i].events & EPOLLOUT && conn->is_alive()) {
                            try {
                                conn->handle_write();
                            } catch (const std::exception &e) {
                                NET_LOG_ERROR("Exception in handle_write for fd {}: {}", fd, e.what());
                                conn->shutdown();
                            }
                        }

                        if (!conn->is_alive()) {
                            close_connection(fd);
                        }
                    }
                } catch (const std::exception &e) {
                    NET_LOG_ERROR("Unhandled exception processing fd {}: {}", fd, e.what());
                    close_connection(fd);
                }
            }

            try {
                if (need_pending) {
                    do_pending_functors();
                }
            } catch (const std::exception &e) {
                NET_LOG_ERROR("Exception in do_pending_functors: {}", e.what());
            }
        }
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Fatal exception in reactor loop: {}", e.what());
    }

    // Cleanup on exit
    std::vector<int> fds;

    for (const auto &fd: connections_ | std::views::keys)
        fds.emplace_back(fd);

    for (const int fd: fds) {
        try {
            close_connection(fd);
        } catch (const std::exception &e) {
            NET_LOG_ERROR("Exception closing fd {} during shutdown: {}", fd, e.what());
        }
    }
}

// Called by connection when output buffer becomes non-empty
void SubReactor::enable_writing(int fd) {
    if (fd < 0) {
        NET_LOG_ERROR("Invalid fd passed to enable_writing");
        return;
    }

    if (std::this_thread::get_id() == thread_.get_id()) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET | EPOLLOUT;
        ev.data.fd = fd;
        epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, fd, &ev);
        return;
    }

    {
        std::lock_guard lock(pending_mutex_);
        pending_functors_.emplace_back([this, fd]() {
            try {
                if (!connections_.contains(fd))
                    return;

                epoll_event ev{};
                ev.events = EPOLLIN | EPOLLET | EPOLLOUT;
                ev.data.fd = fd;
                if (epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, fd, &ev) < 0) {
                    if (errno != ENOENT && errno != EBADF) {
                        NET_LOG_WARN("Epoll_mod enable_write failed for fd {}: {}", fd, strerror(errno));
                    }
                }
            } catch (const std::exception &e) {
                NET_LOG_ERROR("Exception in enable_writing functor for fd {}: {}", fd, e.what());
            }
        });
    }

    constexpr uint64_t one = 1;
    ::write(wake_fd_, &one, sizeof(one));
}

void SubReactor::disable_writing(int fd) {
    if (fd < 0) {
        NET_LOG_ERROR("Invalid fd passed to disable_writing");
        return;
    }

    if (std::this_thread::get_id() == thread_.get_id()) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = fd;
        epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, fd, &ev);
        return;
    }

    {
        std::lock_guard lock(pending_mutex_);
        pending_functors_.emplace_back([this, fd]() {
            try {
                if (!connections_.contains(fd))
                    return;

                epoll_event ev{};
                ev.events = EPOLLIN | EPOLLET; // Remove EPOLLOUT
                ev.data.fd = fd;
                if (epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, fd, &ev) < 0) {
                    if (errno != ENOENT && errno != EBADF) {
                        NET_LOG_WARN("Epoll_mod disable_write failed for fd {}: {}", fd, strerror(errno));
                    }
                }
            } catch (const std::exception &e) {
                NET_LOG_ERROR("Exception in disable_writing functor for fd {}: {}", fd, e.what());
            }
        });
    }
}

void SubReactor::close_connection(const int fd) {
    if (fd < 0) {
        return;
    }

    try {
        const auto it = connections_.find(fd);
        if (it == connections_.end()) {
            return;
        }
        const std::shared_ptr<EpollConnection> conn = std::move(it->second);
        connections_.erase(it);

        for (auto pair = timeout_map_.begin(); pair != timeout_map_.end();) {
            if (pair->second == fd) {
                pair = timeout_map_.erase(pair);
            } else {
                ++pair;
            }
        }

        if (conn) {
            try {
                net_utils::epoll_del(epoll_fd_.get(), fd);
            } catch (const std::exception &e) {
                NET_LOG_WARN("Failed to remove fd {} from epoll: {}", fd, e.what());
            }

            try {
                conn->shutdown();
            } catch (const std::exception &e) {
                NET_LOG_ERROR("Exception during connection shutdown for fd {}: {}", fd, e.what());
            }
        }
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Exception in close_connection for fd {}: {}", fd, e.what());
    }
}

// Close connections idle longer than configured timeout
void SubReactor::check_timeouts() {
    const auto now = std::chrono::steady_clock::now();

    try {
        std::vector<int> timeouts_fds;

        auto it = timeout_map_.begin();
        while (it != timeout_map_.end() && it->first <= now) {
            int fd = it->second;
            it = timeout_map_.erase(it);

            if (auto conn_it = connections_.find(fd);
                conn_it != connections_.end() && conn_it->second->is_idle_timeout_enabled()) {
                if (now - conn_it->second->get_last_active() > conn_it->second->get_idle_timeout()) {
                    timeouts_fds.emplace_back(fd);
                } else {
                    auto next_timeout = conn_it->second->get_last_active() + conn_it->second->get_idle_timeout();
                    timeout_map_.emplace(next_timeout, fd);
                }
            }
        }

        for (int fd: timeouts_fds) {
            std::shared_ptr<EpollConnection> conn;

            if (auto pair = connections_.find(fd); pair != connections_.end())
                conn = pair->second;

            if (conn) {
                NET_LOG_WARN("Idle timeout for {}, closing", conn->get_peer_info());
                try {
                    conn->shutdown();
                } catch (const std::exception &e) {
                    NET_LOG_ERROR("Exception shutting down timed out connection: {}", e.what());
                }
            }
        }
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Exception in check_timeouts: {}", e.what());
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
        try {
            if (f) {
                f();
            }
        } catch (const std::exception &e) {
            NET_LOG_ERROR("Exception executing pending functor: {}", e.what());
        } catch (...) {
            NET_LOG_ERROR("Unknown exception executing pending functor");
        }
    }
}

void SubReactor::shutdown() {
    if (bool expected = false; !shutdown_done_.compare_exchange_strong(expected, true))
        return;

    try {
        if (wake_fd_ != -1) {
            constexpr uint64_t one = 1;
            while (::write(wake_fd_, &one, sizeof(one)) < 0 && errno == EINTR)
                ;
        }

        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }

        {
            // std::unique_lock lock(connection_mutex_);
            for (auto &[fd, conn]: connections_) {
                try {
                    net_utils::epoll_del(epoll_fd_.get(), fd);
                } catch (const std::exception &e) {
                    NET_LOG_WARN("Failed to remove fd {} from epoll during shutdown: {}", fd, e.what());
                }

                try {
                    if (conn) {
                        conn->shutdown();
                    }
                } catch (const std::exception &e) {
                    NET_LOG_ERROR("Exception shutting down connection fd {}: {}", fd, e.what());
                }
            }
            connections_.clear();
            timeout_map_.clear();
        }

        net_utils::close_safe(wake_fd_);
        wake_fd_ = -1;

        net_utils::close_safe(timer_fd_);
        timer_fd_ = -1;
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Exception during SubReactor shutdown: {}", e.what());
    }
}
