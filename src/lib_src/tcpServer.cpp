#include "../../include/tcpServer.h"
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <sys/epoll.h>
#include <unistd.h>

std::atomic<TcpServer *> TcpServer::instance_{nullptr};
std::mutex TcpServer::instance_mutex_;

void TcpServer::signal_handler(int sig) {
    if (const auto *inst = instance()) {
        inst->stop_source_.request_stop();
        inst->wakeup();
    }
}

TcpServer::TcpServer(const int port, const int backlog, const bool use_epoll, const int num_reactors) :
    port_(port), backlog_(backlog), use_epoll_(use_epoll), num_reactors_(num_reactors) {

    {
        std::lock_guard lock(instance_mutex_);
        if (instance_.load(std::memory_order_relaxed) != nullptr) {
            NET_LOG_WARN("Multiple instances of TcpServer detected,signal handling may be unreliable");
        } else {
            instance_.store(this, std::memory_order_release);
        }
    }

    if (pipe(wakeup_pipe_) < 0) {
        throw std::runtime_error("Failed to create wakeup pipe");
    }
    net_utils::set_nonblocking(wakeup_pipe_[0]);
    net_utils::set_nonblocking(wakeup_pipe_[1]);

    // Create listening socket
    server_fd_ = net_utils::make_socket_raii(AF_INET, SOCK_STREAM, 0);

    // Allow port reuse immediately after release
    net_utils::set_reuse_addr(server_fd_->get());

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port_);

    // Bind to any address:port
    net_utils::check_syscall(bind(server_fd_->get(), reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)),
                             "bind");

    // Start listening for incoming connections
    net_utils::check_syscall(listen(server_fd_->get(), backlog_), "listen");

    std::signal(SIGINT, &TcpServer::signal_handler);
    std::signal(SIGTERM, &TcpServer::signal_handler);

    NET_LOG_INFO("Server initialized on port {}", port_);
}

TcpServer::TcpServer(int port, int backlog, bool use_epoll) : TcpServer(port, backlog, use_epoll, 1) {
    instance_ = this;
}

TcpServer::~TcpServer() {
    shutdown();
    {
        std::lock_guard lock(instance_mutex_);
        if (instance_.load(std::memory_order_relaxed) == this) {
            instance_.store(nullptr, std::memory_order_release);
        }
    }
}

// Start server: choose epoll/blocking mode
void TcpServer::start(const ClientHandler &handler) {
    client_handler_ = handler;

    if (!use_epoll_) {
        accept_thread_ = std::jthread([this](const std::stop_token &st) { start_blocking(client_handler_, st); });
    }
    if (num_reactors_ <= 1) {
        accept_thread_ = std::jthread([this](const std::stop_token &st) { start_single_epoll(client_handler_, st); });
    } else {
        for (int i = 0; i < num_reactors_; ++i) {
            sub_reactors_.emplace_back(std::make_unique<SubReactor>(client_handler_));
        }

        accept_thread_ = std::jthread([this](const std::stop_token &st) { start_multi_epoll(client_handler_, st); });
    }
}

// blocking mode
void TcpServer::start_blocking(const ClientHandler &handler, const std::stop_token &st) const {
    fd_set readfds;
    const int max_fd = std::max(server_fd_->get(), wakeup_pipe_[0]);

    while (!st.stop_requested()) {
        FD_ZERO(&readfds);
        FD_SET(server_fd_->get(), &readfds);
        FD_SET(wakeup_pipe_[0], &readfds);

        struct timeval tv{0, 200000}; // 200ms
        if (const int ret = select(max_fd + 1, &readfds, nullptr, nullptr, &tv); ret < 0) {
            if (errno == EINTR)
                continue;
            NET_LOG_ERROR("select failed in blocking mode: {}", strerror(errno));
            break;
        }

        if (FD_ISSET(wakeup_pipe_[0], &readfds)) {
            char buf[16];
            while (::read(wakeup_pipe_[0], buf, sizeof(buf)) > 0) {
            }
            NET_LOG_INFO("Shutdown signal received in blocking mode");
            continue;
        }

        if (FD_ISSET(server_fd_->get(), &readfds)) {
            handle_accept(server_fd_->get(), handler);
        }
    }
}

// Add epoll mode
void TcpServer::start_single_epoll(const ClientHandler &handler, const std::stop_token &st) {
    auto reactor = std::make_unique<SubReactor>(handler);
    sub_reactors_.emplace_back(std::move(reactor));

    const net_utils::EpollFd accept_fd;
    net_utils::epoll_add(accept_fd.get(), server_fd_->get(), EPOLLIN);
    net_utils::epoll_add(accept_fd.get(), wakeup_pipe_[0], EPOLLIN);

    epoll_event events[2] = {};

    while (!st.stop_requested()) {
        const int nfds = epoll_wait(accept_fd.get(), events, 2, -1);
        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            NET_LOG_ERROR("epoll_wait failed in single reactor: {}", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            const int fd = events[i].data.fd;

            if (fd == wakeup_pipe_[0]) {
                char buf[16];
                while (::read(wakeup_pipe_[0], buf, sizeof(buf)) > 0) {
                }
                NET_LOG_INFO("Shutdown signal received in single reactor");
                continue;
            }

            if (fd == server_fd_->get()) {
                while (!st.stop_requested()) {
                    handle_accept(fd, handler);
                }
            }
        }
    }

    NET_LOG_INFO("Single reactor accept loop running");
}

void TcpServer::start_multi_epoll(const ClientHandler &handler, const std::stop_token &st) {
    NET_LOG_INFO("start_multi_reactor with {} reactors", num_reactors_);

    const net_utils::EpollFd accept_fd;
    net_utils::epoll_add(accept_fd.get(), server_fd_->get(), EPOLLIN);
    net_utils::epoll_add(accept_fd.get(), wakeup_pipe_[0], EPOLLIN);

    epoll_event events[2] = {};

    while (!st.stop_requested()) {
        const int nfds = epoll_wait(accept_fd.get(), events, 2, -1);
        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            NET_LOG_ERROR("Epoll wait failed in multi-reactor: {}", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            const int fd = events[i].data.fd;

            if (fd == wakeup_pipe_[0]) {
                char buf[16];
                while (::read(wakeup_pipe_[0], buf, sizeof(buf)) > 0) {
                }
                NET_LOG_INFO("Shutdown signal received in multi-reactor");
                continue;
            }

            if (fd == server_fd_->get()) {
                while (!st.stop_requested()) {
                    sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);

                    const int conn_fd = accept4(server_fd_->get(), reinterpret_cast<sockaddr *>(&client_addr), &client_len,
                                          SOCK_NONBLOCK);
                    if (conn_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        if (errno == EINTR)
                            continue;
                        NET_LOG_ERROR("Accept4 failed: {}", strerror(errno));
                        break;
                    }

                    net_utils::SocketPtr client_fd{new net_utils::SocketFd(conn_fd), net_utils::SocketDeleter{}};

                    char client_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
                    NET_LOG_INFO("Client connected in multi-reactor: {}:{}", client_ip, ntohs(client_addr.sin_port));

                    if (sub_reactors_.empty())
                        break;
                    size_t index = next_reactor_index_++ % num_reactors_;
                    sub_reactors_[index]->add_connection(std::move(client_fd), client_addr);
                }
            }
        }
    }
}

void TcpServer::handle_accept(const int listen_fd, const ClientHandler &handler) const {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    const int client_raw_fd = accept(listen_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
    if (client_raw_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            NET_LOG_ERROR("Accept failed: {}", strerror(errno));
        }
        return;
    }
    net_utils::set_nonblocking(client_raw_fd);

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    NET_LOG_INFO("New connection from {}", client_ip, ntohs(client_addr.sin_port));

    try {
        net_utils::SocketPtr client_fd{new net_utils::SocketFd(client_raw_fd), net_utils::SocketDeleter{}};

        if (!sub_reactors_.empty()) {
            sub_reactors_[0]->add_connection(std::move(client_fd), client_addr);
        } else {
            handler(std::move(client_fd), client_addr);
        }
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Failed to accept connection: {}", e.what());
        net_utils::close_safe(client_raw_fd);
    }
}

void TcpServer::wakeup() const {
    if (wakeup_pipe_[1] >= 0) {
        constexpr char c = 'S';
        ::write(wakeup_pipe_[1], &c, 1);
    }
}

// Trigger shutdown from anywhere (thread-safe)
void TcpServer::shutdown() {
    for (const auto &reactor: sub_reactors_) {
        reactor->shutdown();
    }
    sub_reactors_.clear();

    stop_source_.request_stop();
    wakeup();

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    server_fd_.reset();

    net_utils::close_safe(wakeup_pipe_[0]);
    net_utils::close_safe(wakeup_pipe_[1]);
    wakeup_pipe_[0] = wakeup_pipe_[1] = -1;

    NET_LOG_INFO("Tcpserver shutdown complete");
}
