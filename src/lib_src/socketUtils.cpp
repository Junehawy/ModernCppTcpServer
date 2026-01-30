#include "../../include/socketUtils.h"
#include <iostream>

// Write exactly n bytes (retry on EINTR, throw on error)
ssize_t writen(int fd, const void *data, size_t n) {
    size_t left = n;
    auto ptr = static_cast<const char *>(data);

    while (left > 0) {
        ssize_t sent = write(fd, ptr, left);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            spdlog::error("writen failed: {}", strerror(errno));
            return -1;
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
    char buf[4096];

    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <0) {
            if (errno == EINTR) continue;
            spdlog::error("read_line failed: {}", strerror(errno));
            return line;
        }
        if (n == 0)
            return line; // EOF

        line.append(buf, n);
        size_t pos = line.find('\n');
        if (pos != std::string::npos) {
            line = line.substr(0, pos);
            return line;
        }
    }
}

// Enable port reuse (avoid "address already in use")
bool set_reuse_addr(int fd) {
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        spdlog::error("setsockopt failed: {}", strerror(errno));
        return false;
    }
    return true;
}

// Graceful close: shutdown + close
void close_fd(int fd) {
    if (fd < 0)
        return;

    if (::close(fd) == -1) {
        if (errno != EBADF && errno != EINTR) {
            spdlog::warn("close fd {} failed: {}", fd, strerror(errno));
        }
    }
}

namespace socket_utils {
    void add_epoll(int epfd,int fd,uint32_t events) {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
            spdlog::error("epoll_ctl add fd {} failed: {}", fd,strerror(errno));
            throw SocketException("epoll_ctl add failed");
        }
    }

    void mod_epoll(int epfd,int fd,uint32_t events) {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == -1) {
            spdlog::error("epoll_ctl mod fd {} failed: {}", fd,strerror(errno));
            throw SocketException("epoll_ctl mod failed");
        }
    }

    void del_epoll(int epfd,int fd) {
        if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr) == -1) {
            spdlog::error("epoll_ctl del fd {} failed: {}", fd, strerror(errno));
        }
    }

    void check_syscall(int ret, const std::string &msg, bool throw_on_fail) {
        if (ret < 0) {
            std::string err = msg + ": " + strerror(errno);
            spdlog::error("{}",err);
            if (throw_on_fail) {
                throw SocketException(err);
            }
        }
    }
} // namespace socket_utils
