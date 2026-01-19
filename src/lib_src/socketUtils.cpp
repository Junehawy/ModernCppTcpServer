#include "../../include/socketUtils.h"
#include <iostream>

// Global shutdown control
std::atomic<bool> g_server_should_stop { false };
std::mutex g_shutdown_mutex;
std::condition_variable g_shutdown_cv;

// Write exactly n bytes (retry on EINTR, throw on error)
ssize_t writen(int fd, const void* data, size_t n) {
    size_t left = n;
    ssize_t sent;
    const char* ptr = static_cast<const char*>(data);

    while (left > 0) {
        sent = write(fd, ptr, left);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            throw SocketException("writen failed", errno);
        }
        if (sent == 0)
            break;

        left -= sent;
        ptr += sent;
    }
    return n - left;
}

// Read until '\n' (one byte at a time)
std::string read_line(int fd) {
    std::string line;
    char ch;

    while (true) {
        ssize_t n = read(fd, &ch, 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw SocketException("read_line error");
        }
        if (n == 0)
            return line; // EOF

        if (ch == '\n')
            break;
        line += ch;
    }
    return line;
}

// Enable port reuse (avoid "address already in use")
bool set_reuse_addr(int fd) {
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        throw SocketException("set_reuse_addr error");
    }
    return true;
}

// Graceful close: shutdown + close
void close_fd(int fd) {
    if (fd < 0)
        return;

    ::shutdown(fd, SHUT_RDWR);

    if (::close(fd) == -1) {
        LOG_ERROR("close fd %d failed: %s", fd, strerror(errno));
    }
}

// Signal handler for clean shutdown
void global_signal_handler(int sig) {
    std::cout << "Received signal " << sig << ", shutdown...\n";

    g_server_should_stop = true;

    {
        std::lock_guard<std::mutex> lock(g_shutdown_mutex);
        g_shutdown_cv.notify_all();
    }
}