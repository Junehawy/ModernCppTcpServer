#include "../../include/tcpServer.h"
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <unistd.h>

TcpServer::TcpServer(const int port, const int backlog, const size_t num_reactors) :
    port_(port), backlog_(backlog), num_reactors_(num_reactors) {

    try {
        event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (event_fd_ < 0) {
            throw std::runtime_error("Failed to create signalfd");
        }
        NET_LOG_INFO("event_fd_ created successfully: {}", event_fd_);

        // Create listening socket
        server_fd_ = net_utils::make_socket_raii(AF_INET, SOCK_STREAM, 0);

        // Allow port reuse immediately after release
        net_utils::set_reuse_addr(server_fd_->get());

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        server_addr.sin_port = htons(port_);

        // Bind to any address:port
        net_utils::check_syscall(
                bind(server_fd_->get(), reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)), "bind");

        // Start listening for incoming connections
        net_utils::check_syscall(listen(server_fd_->get(), backlog_), "listen");

        NET_LOG_INFO("Server initialized on port {}", port_);
    } catch (...) {
        // Cleanup on construction failure
        if (event_fd_ >= 0) {
            net_utils::close_safe(event_fd_);
            event_fd_ = -1;
        }
        server_fd_.reset();
        throw;
    }
}

TcpServer::~TcpServer() {

    try {
        shutdown();
    } catch (const std::exception &e) {
        // Never throw from destructor
        NET_LOG_ERROR("Exception in TcpServer destructor: {}", e.what());
    }

    net_utils::close_safe(event_fd_);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
}

// Start server: choose epoll/blocking mode
void TcpServer::start(const ClientHandler &handler) {
    if (!handler) {
        throw std::invalid_argument("Client handler cannot be null");
    }

    try {
        client_handler_ = handler;
        running_ = true;

        auto token = get_stop_token();

        if (num_reactors_ <= 1) {
            accept_thread_ = std::jthread([this](std::stop_token) {
                try {
                    start_single_epoll(stop_source_.get_token());
                } catch (const std::exception &e) {
                    NET_LOG_ERROR("Exception in single epoll mode: {}", e.what());
                    running_ = false;
                }
            });
        } else {
            try {
                for (size_t i = 0; i < num_reactors_; ++i) {
                    sub_reactors_.emplace_back(std::make_unique<SubReactor>(client_handler_));
                }
            } catch (const std::exception &e) {
                NET_LOG_ERROR("Failed to create sub-reactors: {}", e.what());
                sub_reactors_.clear(); // Cleanup partial reactors
                throw;
            }

            accept_thread_ = std::jthread([this](std::stop_token) {
                try {
                    start_multi_epoll(stop_source_.get_token());
                } catch (const std::exception &e) {
                    NET_LOG_ERROR("Exception in multi epoll mode: {}", e.what());
                    running_ = false;
                }
            });
        }

    } catch (const std::exception &e) {
        running_ = false;
        throw;
    }
}

// single epoll mode
void TcpServer::start_single_epoll(std::stop_token st) {
    NET_LOG_INFO("Start singal reactor mode");

    try {
        auto reactor = std::make_unique<SubReactor>(client_handler_);
        sub_reactors_.emplace_back(std::move(reactor));

        const net_utils::EpollFd accept_fd;
        net_utils::epoll_add(accept_fd.get(), server_fd_->get(), EPOLLIN);
        net_utils::epoll_add(accept_fd.get(), event_fd_, EPOLLIN);

        epoll_event events[2] = {};


        while (!st.stop_requested() && running_.load()) {
            const int nfds = epoll_wait(accept_fd.get(), events, 2, -1);
            if (nfds < 0) {
                if (errno == EINTR)
                    continue;
                NET_LOG_ERROR("epoll_wait failed in single reactor: {}", strerror(errno));
                break;
            }

            for (int i = 0; i < nfds; i++) {
                const int fd = events[i].data.fd;

                if (fd == event_fd_) {
                    uint64_t count;
                    while (read(event_fd_, &count, sizeof(count)) > 0) {
                    }
                    NET_LOG_DEBUG("Wakeup received");
                    continue;
                }

                if (fd == server_fd_->get()) {
                    size_t accept_count = 0;
                    while (!st.stop_requested() && running_.load() && accept_count < MAX_ACCEPT_PER_LOOP) {
                        try {
                            if (!handle_accept())
                                break;
                            accept_count++;
                        } catch (const std::exception &e) {
                            NET_LOG_ERROR("Exception in accept loop: {}", e.what());
                            break;
                        }
                    }
                }
            }
        }

        NET_LOG_INFO("Single reactor accept loop exiting");
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Fatal exception in single reactor mode: {}", e.what());
    }
}

void TcpServer::start_multi_epoll(std::stop_token st) {
    NET_LOG_INFO("Start_multi_reactor with {} reactors", num_reactors_);

    try {
        const net_utils::EpollFd accept_fd;
        net_utils::epoll_add(accept_fd.get(), server_fd_->get(), EPOLLIN);
        net_utils::epoll_add(accept_fd.get(), event_fd_, EPOLLIN);

        epoll_event events[2] = {};

        while (!st.stop_requested() && running_.load()) {
            const int nfds = epoll_wait(accept_fd.get(), events, 2, 100);
            if (nfds < 0) {
                if (errno == EINTR)
                    continue;
                NET_LOG_ERROR("Epoll wait failed in multi-reactor: {}", strerror(errno));
                break;
            }

            for (int i = 0; i < nfds; i++) {
                const int fd = events[i].data.fd;

                if (fd == event_fd_) {
                    uint64_t count;
                    while (read(event_fd_, &count, sizeof(count)) > 0) {
                    }
                    NET_LOG_DEBUG("Wakeup received");
                    continue;
                }

                if (fd == server_fd_->get()) {
                    try {
                        sockaddr_in client_addr{};
                        socklen_t client_len = sizeof(client_addr);

                        size_t accept_count = 0;
                        while (accept_count < MAX_ACCEPT_PER_LOOP && !st.stop_requested() && running_.load()) {
                            const int conn_fd = accept4(server_fd_->get(), reinterpret_cast<sockaddr *>(&client_addr),
                                                        &client_len, SOCK_NONBLOCK);
                            if (conn_fd == -1) {
                                if (errno == EAGAIN || errno == EWOULDBLOCK)
                                    break;
                                if (errno == EINTR)
                                    continue;
                                NET_LOG_ERROR("Accept4 failed: {}", strerror(errno));
                                break;
                            }

                            try {
                                net_utils::SocketPtr client_fd{new net_utils::SocketFd(conn_fd),
                                                               net_utils::SocketDeleter{}};

                                char client_ip[INET_ADDRSTRLEN];
                                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
                                NET_LOG_INFO("Client connected in multi-reactor: {}:{}", client_ip,
                                             ntohs(client_addr.sin_port));

                                if (sub_reactors_.empty()) {
                                    net_utils::close_safe(conn_fd);
                                    break;
                                }

                                size_t index = next_reactor_index_++ % num_reactors_;
                                sub_reactors_[index]->add_connection(std::move(client_fd), client_addr);
                                accept_count++;
                            } catch (const std::exception &e) {
                                NET_LOG_ERROR("Exception adding connection: {}", e.what());
                                net_utils::close_safe(conn_fd);
                            }

                            if (accept_count > 0) {
                                std::this_thread::yield();
                            } else {
                                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                            }
                        }
                    } catch (const std::exception &e) {
                        NET_LOG_ERROR("Exception in accept loop: {}", e.what());
                    }
                    break;
                }
            }
        }
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Fatal exception in multi-reactor mode: {}", e.what());
    }

    NET_LOG_INFO("Multi-reactor accept loop exiting");
}

bool TcpServer::handle_accept() const {
    try {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        const int client_raw_fd =
                accept4(server_fd_->get(), reinterpret_cast<sockaddr *>(&client_addr), &client_len, SOCK_NONBLOCK);
        if (client_raw_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            if (errno == EINTR) {
                return false;
            }
            NET_LOG_ERROR("Accept4 failed: {}", strerror(errno));
            return false;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        NET_LOG_INFO("New connection from {}:{}", client_ip, ntohs(client_addr.sin_port));

        try {
            net_utils::SocketPtr client_fd{new net_utils::SocketFd(client_raw_fd), net_utils::SocketDeleter{}};

            sub_reactors_[0]->add_connection(std::move(client_fd), client_addr);

            return true;
        } catch (const std::exception &e) {
            NET_LOG_ERROR("Failed to accept connection: {}", e.what());
            net_utils::close_safe(client_raw_fd);

            return false;
        }
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Exception in handle_accept: {}", e.what());

        return false;
    }
}

// Trigger shutdown from anywhere (thread-safe)
void TcpServer::shutdown() {
    if (!running_.exchange(false))
        return;

    NET_LOG_DEBUG("=== Graceful shutdown started ===");

    try {
        stop_source_.request_stop();

        NET_LOG_DEBUG("Closing listening socket...");
        server_fd_.reset();

        wakeup();

        if (accept_thread_.joinable()) {
            NET_LOG_INFO("Waiting for accept thread...");
            accept_thread_.join();
        }

        NET_LOG_DEBUG("Stopping {} sub-reactors...", sub_reactors_.size());
        for (auto &reactor: sub_reactors_) {
            if (reactor)
                reactor->shutdown();
        }
        sub_reactors_.clear();


        net_utils::close_safe(event_fd_);
        event_fd_ = -1;

        NET_LOG_DEBUG("=== Graceful shutdown complete ===");
        spdlog::default_logger()->flush();
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Exception during shutdown: {}", e.what());
    }
}

void TcpServer::wakeup() const {
    try {
        uint64_t one = 1;
        // wake accept
        if (event_fd_ >= 0) {
            ::write(event_fd_, &one, sizeof(one));
        }
        // wake sub-reactor
        for (auto &reactor: sub_reactors_) {
            if (reactor && reactor->get_wake_fd() >= 0) {
                ::write(reactor->get_wake_fd(), &one, sizeof(one));
            }
        }
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Exception in wakeup: {}", e.what());
    }
}
