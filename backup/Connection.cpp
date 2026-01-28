#include "../include/Connection.h"
#include <cstring>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

Connection::Connection(SocketPtr sock, sockaddr_in addr, MessageHandler handler)
    : socket_(std::move(sock))
      , client_addr_(addr)
      , message_handler_(std::move(handler)) {
}

Connection::~Connection() {
    shutdown();
}

// Start a background thread to handle this connection
void Connection::start() {
    worker_ = std::jthread([this] { run(); });
}

void Connection::shutdown() {
    if (!running_.exchange(false, std::memory_order_acq_rel))
        return;

    if (socket_ && socket_->valid()) {
        int fd = socket_->get();

        linger ling{};
        ling.l_onoff = 1;
        ling.l_linger = 0;
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));

        ::shutdown(fd, SHUT_RDWR);
        socket_.reset();
    }
    read_buffer_.clear();
    write_buffer_.clear();
}

bool Connection::is_alive() const {
    return running_.load(std::memory_order_acquire);
}

ssize_t Connection::send(const std::string &msg) {
    if (!is_alive())
        return -1;

    write_buffer_ += msg;
    return msg.size();
}

std::string Connection::get_peer_info() const {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr_.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(client_addr_.sin_port));
}

// Main loop: read from client, parse lines, dispatch messages
void Connection::run() {
    char temp[READ_CHUNK_SIZE];
    read_buffer_.reserve(4096); // Reduce reallocations

    while (running_.load(std::memory_order_acquire)) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(socket_->get(), &readfds);

        struct timeval tv{0, 200000}; // 200ms timeout

        int ret = ::select(socket_->get() + 1, &readfds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            spdlog::error("{} select error: {}", get_peer_info(), strerror(errno));
            break;
        }

        // Timeout → check idle timeout if enabled
        if (ret == 0) {
            if (enable_idle_timeout_ && (std::chrono::steady_clock::now() - last_active_ > idle_timeout_)) {
                spdlog::warn("Idle timeout ({}s) for {}, closing",
                             idle_timeout_.count(), get_peer_info());
                shutdown();
                break;
            }
            continue;
        }

        if (!FD_ISSET(socket_->get(), &readfds))
            continue;

        // Read data from socket
        ssize_t n = ::read(socket_->get(), temp, sizeof(temp));
        if (n <= 0) {
            if (n < 0)
                spdlog::error("{} read error: {}", get_peer_info(), strerror(errno));
            else
                spdlog::info("{} disconnected.", get_peer_info());
            break;
        }

        last_active_ = std::chrono::steady_clock::now();
        read_buffer_.append(temp, n);

        // Process complete lines (ended with \n)
        size_t pos;
        while ((pos = read_buffer_.find('\n')) != std::string::npos) {

            std::string line = read_buffer_.substr(0, pos);
            read_buffer_.erase(0, pos + 1);

            // Remove trailing \r if present (Windows-style line ending)
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (message_handler_) {
                message_handler_(this, std::move(line));
            }
        }
    }

    // Cleanup before thread exits
    shutdown();
    running_.store(false, std::memory_order_release);
}

int Connection::get_fd() const {
    return socket_ ? socket_->get() : -1;
}

void Connection::handle_read() {
    char temp[4096];
    bool has_read = false;

    for (int attempt =0; attempt < 5; attempt++) {
        while (true) {
            ssize_t n = ::read(get_fd(), temp, sizeof(temp));
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break; // read over
                spdlog::error("{} read error: {}", get_peer_info(), strerror(errno));
                shutdown();
                break;
            }
            if (n == 0) {
                spdlog::debug("{} client disconnected", get_peer_info());
                shutdown();
                return;
            }

            read_buffer_.append(temp, n);
            has_read = true;
            update_active_time();
            spdlog::debug("{} read {} bytes (buffer now {})", get_peer_info(), n, read_buffer_.size());
        }
        if (has_read) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    size_t pos;
    while ((pos = read_buffer_.find('\n')) != std::string::npos) {
        std::string line = read_buffer_.substr(0, pos);
        read_buffer_.erase(0, pos + 1);

        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (message_handler_)
            message_handler_(this, line);
    }
}

void Connection::handle_write() {
    while (!write_buffer_.empty()) {
        ssize_t n = ::write(get_fd(), write_buffer_.data(), write_buffer_.size());
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; // write over
            spdlog::error("{} write error: {}", get_peer_info(), strerror(errno));
            shutdown();
            break;
        }
        write_buffer_.erase(0, n);
    }
}

void Connection::handle_error() {
    spdlog::error("{} connection error,closing", get_peer_info());
    shutdown();
}

void Connection::update_active_time() {
    last_active_ = std::chrono::steady_clock::now();
}
