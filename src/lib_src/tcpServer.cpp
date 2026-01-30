#include "../../include/tcpServer.h"
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <unordered_map>

int TcpServer::s_wakeup_pipe_[2] = {-1, -1};
std::once_flag TcpServer::s_pipe_init_flag;

// RAII wrapper for epoll fd
class EpollFd {
public:
    explicit EpollFd(int flags = 0) : fd_(epoll_create1(flags)) {
        if (fd_ < 0) {
            throw SocketException("epoll_create1 failed", errno);
        }
    }
    ~EpollFd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    EpollFd(const EpollFd &) = delete;
    EpollFd &operator=(const EpollFd &) = delete;

    EpollFd(EpollFd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    EpollFd &operator=(EpollFd &&other) noexcept {
        if (this != &other) {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; };
    operator int() const noexcept { return fd_; }

private:
    int fd_ = -1;
};

void TcpServer::init_wakeup_pipe() {
    if (pipe(s_wakeup_pipe_) == -1) {
        throw SocketException("pipe failed for wakeup", errno);
    }
    fcntl(s_wakeup_pipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(s_wakeup_pipe_[1], F_SETFL, O_NONBLOCK);
}

void TcpServer::signal_handler(int sig) {
    char c = 'S';
    ::write(s_wakeup_pipe_[1], &c, 1);
}

TcpServer::TcpServer(int port, int backlog, bool use_epoll, int num_reactors) :
    server_fd_(nullptr), port_(port), backlog_(backlog), use_epoll_(use_epoll), num_reactors_(num_reactors) {
    // Create listening socket
    server_fd_ = make_socket_raii(AF_INET, SOCK_STREAM, 0);

    // Allow port reuse immediately after release
    if (!set_reuse_addr(server_fd_->get())) {
        throw SocketException("set_reuse_addr failed");
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port_);

    // Bind to any address:port
    if (bind(server_fd_->get(), reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)) == -1) {
        throw SocketException("bind failed");
    }

    // Start listening for incoming connections
    if (listen(server_fd_->get(), backlog_) == -1) {
        throw SocketException("listen failed");
    }

    spdlog::info("Server listening on port {}", port_);

    std::call_once(s_pipe_init_flag, &TcpServer::init_wakeup_pipe);
    std::signal(SIGINT, &TcpServer::signal_handler);
    std::signal(SIGTERM, &TcpServer::signal_handler);
}

TcpServer::TcpServer(int port, int backlog, bool use_epoll) : TcpServer(port, backlog, use_epoll, 1) {}

TcpServer::~TcpServer() {
    shutdown();
    spdlog::info("TcpServer destroyed.");
}

// Start server: choose epoll/blocking mode
void TcpServer::start(const ClientHandler &handler) {
    if (use_epoll_) {
        start_epoll(handler);
    } else {
        start_blocking(handler);
    }
}

// blocking mode
void TcpServer::start_blocking(const ClientHandler &handler) {
    // Accept loop runs in dedicated thread
    accept_thread_ = std::jthread([this, handler] {
        fd_set readfds;
        int max_fd = std::max(server_fd_->get(), s_wakeup_pipe_[0]);

        while (running_ && !should_stop_) {
            FD_ZERO(&readfds);
            FD_SET(server_fd_->get(), &readfds);
            FD_SET(s_wakeup_pipe_[0], &readfds);

            struct timeval tv{0, 200000}; // 200ms
            int ret = select(max_fd + 1, &readfds, nullptr, nullptr, &tv);
            if (ret < 0) {
                if (errno == EINTR)
                    continue;
                spdlog::error("select failed in blocking mode: {}", strerror(errno));
                break;
            }

            if (FD_ISSET(s_wakeup_pipe_[0], &readfds)) {
                char buf[16];
                while (::read(s_wakeup_pipe_[0], buf, sizeof(buf)) > 0) {
                }
                spdlog::info("Shutdown signal received in blocking mode");
                should_stop_ = true;
                break;
            }

            if (FD_ISSET(server_fd_->get(), &readfds)) {
                sockaddr_in client_addr{};
                socklen_t client_len = sizeof(client_addr);

                int client_raw_fd = accept(server_fd_->get(), reinterpret_cast<sockaddr *>(&client_addr), &client_len);

                if (client_raw_fd == -1) {
                    if (errno == EINTR || should_stop_)
                        break;
                    spdlog::error("Accept failed: {}", strerror(errno));
                    continue;
                }

                // Wrap raw fd with RAII socket pointer
                SocketPtr client_fd{new SocketFd(client_raw_fd), SocketDeleter{}};

                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

                spdlog::info("Client connected: {}:{}", client_ip, ntohs(client_addr.sin_port));

                // Hand over to user-provided handler (usually creates Connection)
                handler(std::move(client_fd), client_addr);
            }
        }
    });

    spdlog::info("Shutdown signal received, stopping accept loop...");

    running_ = false;
    should_stop_ = true;

    if (server_fd_) {
        ::shutdown(server_fd_->get(), SHUT_RDWR);
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

// Add epoll mode
void TcpServer::start_epoll(const ClientHandler &handler) {
    if (num_reactors_ > 1) {
        start_multi_reactor(handler);
    } else {
        EpollFd epoll_fd(0);
        int epfd = epoll_fd.get();

        // nonblock listen_fd
        fcntl(server_fd_->get(), F_SETFL, fcntl(server_fd_->get(), F_GETFL, 0) | O_NONBLOCK);

        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = server_fd_->get();
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd_->get(), &ev) == -1) {
            throw SocketException("epoll_ctl add server_fd failed");
        }

        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = s_wakeup_pipe_[0];
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, s_wakeup_pipe_[0], &ev) == -1) {
            throw SocketException("epoll_ctl add wakeup_pipe failed");
        }

        spdlog::info("epoll mode enabled for server");

        auto last_check = std::chrono::steady_clock::now();

        while (running_ && !should_stop_.load()) {
            epoll_event events[EPOLL_MAX_EVENTS];
            int nfds = epoll_wait(epfd, events, EPOLL_MAX_EVENTS, EPOLL_WAIT_TIMEOUT.count());
            if (nfds < 0) {
                if (errno == EINTR)
                    continue;
                spdlog::error("epoll_wait failed: {}", strerror(errno));
                break;
            }

            for (int i = 0; i < nfds; i++) {
                int fd = events[i].data.fd;

                // Handle shutdown signal
                if (fd == s_wakeup_pipe_[0]) {
                    char buf[16];
                    while (::read(s_wakeup_pipe_[0], buf, sizeof(buf)) > 0)
                        ;
                    should_stop_ = true;
                    spdlog::info("Shutdown signal received, stopping accept loop...");
                    continue;
                }
                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    spdlog::error("epoll error on fd {}", fd);
                    if (connections_.contains(fd)) {
                        connections_[fd]->handle_error();
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                        connections_.erase(fd);
                    }
                    continue;
                }

                // Accept all pending connections
                if (fd == server_fd_->get()) {
                    while (true) {
                        sockaddr_in client_addr{};
                        socklen_t client_len = sizeof(client_addr);
                        int client_raw_fd =
                                accept(server_fd_->get(), reinterpret_cast<sockaddr *>(&client_addr), &client_len);

                        if (client_raw_fd < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK)
                                break;
                            spdlog::error("accept failed: {}", strerror(errno));
                            break;
                        }

                        // Set client socket non-blocking for ET epoll
                        fcntl(client_raw_fd, F_SETFL, fcntl(client_raw_fd, F_GETFL, 0) | O_NONBLOCK);

                        SocketPtr clientfd{new SocketFd(client_raw_fd), SocketDeleter{}};
                        auto conn = handler(std::move(clientfd), client_addr);
                        if (conn) {
                            int conn_fd = conn->get_fd();
                            auto epoll_conn = std::dynamic_pointer_cast<EpollConnection>(conn);
                            if (!epoll_conn) {
                                throw std::runtime_error("epoll connection is null");
                            }
                            connections_[conn_fd] = epoll_conn;

                            ev.events = EPOLLIN | EPOLLET;
                            ev.data.fd = conn_fd;
                            if (epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &ev) == -1) {
                                spdlog::error("epoll_ctl add client fd {} failed: {}", conn_fd, strerror(errno));
                                connections_.erase(conn_fd);
                            }
                        }
                    }
                    continue;
                }

                // Handle I/O events on existing client connections
                if (connections_.contains(fd)) {
                    if (events[i].events & EPOLLIN) {
                        connections_[fd]->handle_read();
                    }
                    if (events[i].events & EPOLLOUT) {
                        connections_[fd]->handle_write();
                    }

                    ev.data.fd = fd;
                    if (connections_[fd]->has_pending_write()) {
                        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                    } else {
                        ev.events = EPOLLIN | EPOLLET;
                    }
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
                }
            }

            // Periodic idle connection cleanup (every 8 seconds)
            auto now = std::chrono::steady_clock::now();
            if (now - last_check >= std::chrono::seconds(8)) {
                last_check = now;
                std::vector<int> to_close;
                for (const auto &pair: connections_) {
                    const auto &conn = pair.second;
                    if (conn->enable_idle_timeout_ && (now - conn->last_active_ > conn->idle_timeout_)) {
                        spdlog::warn("Idle timeout {}s for {}, closing", conn->idle_timeout_.count(),
                                     conn->get_peer_info());
                        to_close.push_back(pair.first);
                    }
                }
                for (int fd: to_close) {
                    if (connections_.contains(fd)) {
                        connections_[fd]->shutdown();
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                        connections_.erase(fd);
                    }
                }
            }
        }

        // Cleanup all remaining connections on shutdown
        for (auto &pair: connections_) {
            pair.second->shutdown();
            epoll_ctl(epfd, EPOLL_CTL_DEL, pair.first, nullptr);
        }
        connections_.clear();
        spdlog::info("epoll mode shutdown complete");
    }
}

// Trigger shutdown from anywhere (thread-safe)
void TcpServer::shutdown() {
    if (!running_.exchange(false))
        return;

    should_stop_ = true;

    // Wake up epoll_wait via pipe
    char c = 'S';
    ::write(s_wakeup_pipe_[1], &c, 1);

    if (server_fd_ && server_fd_->valid()) {
        ::shutdown(server_fd_->get(), SHUT_RDWR);
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

void TcpServer::start_multi_reactor(const ClientHandler &handler) {
    spdlog::info("start_multi_reactor with {} reactors", num_reactors_);

    std::call_once(s_pipe_init_flag, &TcpServer::init_wakeup_pipe);

    char buf[64];
    while (::read(s_wakeup_pipe_[0], buf, sizeof(buf)) > 0) {
        spdlog::debug("Cleared data from wakeup pipe");
    }

    epoll_fds_.resize(num_reactors_);
    connections_per_reactor_.resize(num_reactors_);

    for (int i = 0; i < num_reactors_; i++) {
        epoll_fds_[i] = epoll_create1(0);
        if (epoll_fds_[i] < 0) {
            throw SocketException("epoll_create1 failed");
        }
    }

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = s_wakeup_pipe_[0];
    if (epoll_ctl(epoll_fds_[0], EPOLL_CTL_ADD, s_wakeup_pipe_[0], &ev) == -1) {
        spdlog::error("epoll ctl add wakeup_pipe failed");
    }

    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd_->get();
    if (epoll_ctl(epoll_fds_[0], EPOLL_CTL_ADD, server_fd_->get(), &ev) == -1) {
        spdlog::error("epoll ctl add server_fd failed");
    }

    fcntl(server_fd_->get(), F_SETFL, fcntl(server_fd_->get(), F_GETFL, 0) | O_NONBLOCK);

    for (int i = 0; i < num_reactors_; i++) {
        reactor_threads_.emplace_back([this, i, handler, epfd = epoll_fds_[i]]() mutable {
            spdlog::info("Reactor {} thread started,epfd={}", i, epfd);

            epoll_event events[EPOLL_MAX_EVENTS];
            auto last_check = std::chrono::steady_clock::now();
            while (running_ && !should_stop_) {
                int nfds = epoll_wait(epfd, events, EPOLL_MAX_EVENTS, 1000);
                if (nfds < 0) {
                    if (errno == EINTR)
                        continue;
                    spdlog::error("epoll_wait failed in reactor: {}", i);
                    break;
                }

                for (int j = 0; j < nfds; j++) {
                    int fd = events[j].data.fd;

                    if (fd == s_wakeup_pipe_[0] && i == 0) {
                        char buf[16];
                        while (::read(s_wakeup_pipe_[0], buf, sizeof(buf)) > 0)
                            ;
                        should_stop_ = true;
                        spdlog::info("Shutdown signal received in reactor 0");
                        continue;
                    }

                    if (fd == server_fd_->get() && i == 0) {
                        while (true) {
                            sockaddr_in client_addr{};
                            socklen_t client_len = sizeof(client_addr);
                            int client_raw_fd =
                                    accept(server_fd_->get(), reinterpret_cast<sockaddr *>(&client_addr), &client_len);

                            if (client_raw_fd < 0) {
                                if (errno == EAGAIN || errno == EWOULDBLOCK)
                                    break;
                                spdlog::error("accept failed: {}", strerror(errno));
                                continue;
                            }

                            fcntl(client_raw_fd, F_SETFL, O_NONBLOCK);

                            SocketPtr clientfd{new SocketFd(client_raw_fd), SocketDeleter{}};
                            auto conn = handler(std::move(clientfd), client_addr);
                            if (conn) {
                                int conn_fd = conn->get_fd();
                                auto epoll_conn = std::dynamic_pointer_cast<EpollConnection>(conn);
                                if (!epoll_conn)
                                    continue;

                                size_t target_reactor = next_reactor_index_++ % num_reactors_;

                                {
                                    {
                                        std::lock_guard<std::mutex> lock(connections_mutex_);
                                        connections_per_reactor_[target_reactor][conn_fd] = epoll_conn;
                                    }
                                }

                                epoll_event ev{};
                                ev.events = EPOLLIN | EPOLLET;
                                ev.data.fd = conn_fd;
                                epoll_ctl(epoll_fds_[target_reactor], EPOLL_CTL_ADD, conn_fd, &ev);
                            }
                        }
                        continue;
                    }
                    std::shared_ptr<EpollConnection> conn;
                    {
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        auto it = connections_per_reactor_[i].find(fd);
                        if (it == connections_per_reactor_[i].end())
                            continue;
                        conn = it->second;
                    }

                    if (events[j].events & (EPOLLHUP | EPOLLERR)) {
                        conn->handle_error();
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        connections_per_reactor_[i].erase(fd);
                        continue;
                    }

                    if (events[j].events & EPOLLIN) {
                        conn->handle_read();
                    }

                    if (events[j].events & EPOLLOUT) {
                        conn->handle_write();
                    }

                    epoll_event ev{};
                    ev.data.fd = fd;
                    ev.events = EPOLLIN | EPOLLET;
                    if (conn->has_pending_write())
                        ev.events |= EPOLLOUT;
                    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
                }
                auto now = std::chrono::steady_clock::now();
                if (now - last_check >= std::chrono::seconds(8)) {
                    last_check = now;
                    std::vector<int> to_close;
                    {
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        for (const auto &pair: connections_per_reactor_[i]) {
                            const auto &conn = pair.second;
                            if (conn->enable_idle_timeout_ && (now - conn->last_active_ > conn->idle_timeout_)) {
                                to_close.push_back(pair.first);
                            }
                        }
                    }
                    for (int fd: to_close) {
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        auto it = connections_per_reactor_[i].find(fd);
                        if (it != connections_per_reactor_[i].end()) {
                            it->second->shutdown();
                            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                            connections_per_reactor_[i].erase(it);
                        }
                    }
                }
            }
            spdlog::info("Reactor {} loop exited", i);
        });
    }

    spdlog::info("All reactor threads started, accept loop running");
    while (running_ && !should_stop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    spdlog::info("Shutdown requested, stopping all reactors");
    for (auto &t: reactor_threads_) {
        if (t.joinable())
            t.join();
    }

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto &map: connections_per_reactor_) {
            for (auto &pair: map) {
                pair.second->shutdown();
            }
            map.clear();
        }
    }

    spdlog::info("Multi-reactor shutdown complete");
}

void TcpServer::distribute_connections(SocketPtr client_fd, const sockaddr_in &client_addr) {
    size_t index = next_reactor_index_++ % sub_reactors_.size();
    sub_reactors_[index]->add_connection(std::move(client_fd), client_addr);
}

void TcpServer::check_idle_timeout(std::unordered_map<int, std::shared_ptr<EpollConnection>> &conns, int epfd) {
    static auto last_check = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();

    if (now - last_check >= std::chrono::seconds(8))
        return;
    last_check = now;

    std::vector<int> to_close;
    for (const auto &pair: conns) {
        const auto &conn = pair.second;
        if (conn->enable_idle_timeout_ && (now - conn->last_active_ > conn->idle_timeout_)) {
            spdlog::warn("Idle timeout {}s for {},closing", conn->idle_timeout_.count(), conn->get_peer_info());
            to_close.push_back(pair.first);
        }
    }

    for (int fd: to_close) {
        if (conns.contains(fd)) {
            conns[fd]->shutdown();
            socket_utils::del_epoll(epfd, fd);
            conns.erase(fd);
        }
    }
}

void TcpServer::cleanup_connections(std::unordered_map<int, std::shared_ptr<EpollConnection>> &conns, int epfd) {
    for (auto &pair: conns) {
        pair.second->shutdown();
        socket_utils::del_epoll(epfd, pair.first);
    }
    conns.clear();
}
