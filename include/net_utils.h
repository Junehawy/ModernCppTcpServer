#pragma once
#include <cstring>
#include <fcntl.h>
#include <format>
#include <netinet/in.h>
#include <source_location>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

#include "spdlog/spdlog.h"

namespace net_utils {
    // Unified exception class
    class SyscallException : public std::runtime_error {
    public:
        SyscallException(const std::string &msg, int err = errno,
                         std::source_location loc = std::source_location::current()) :
            std::runtime_error(msg), err_(err), loc_(loc) {}

        int error_code() const { return err_; }

        const char *what() const noexcept override {
            static thread_local std::string buf;
            buf = std::format("[{}:{} {}] {} (errno:{} {})", loc_.file_name(), loc_.line(), loc_.function_name(),
                              std::runtime_error::what(), err_, strerror(err_));
            return buf.c_str();
        }

    private:
        int err_;
        std::source_location loc_;
    };

// Log
#define NET_LOG_ERROR(fmt, ...) spdlog::error(fmt, ##__VA_ARGS__)
#define NET_LOG_WARN(fmt, ...) spdlog::warn(fmt, ##__VA_ARGS__)
#define NET_LOG_INFO(fmt, ...) spdlog::info(fmt, ##__VA_ARGS__)
#define NET_LOG_DEBUG(fmt, ...) spdlog::debug(fmt, ##__VA_ARGS__)

    inline void check_syscall(int ret, const std::string &op,
                              std::source_location loc = std::source_location::current(), bool fatal = true) {
        if (ret >= 0)
            return;

        int e = errno;
        std::string desc = (e == 0) ? "N/A" : strerror(e);
        std::string msg = op + " failed: " + desc;

        if (fatal) {
            NET_LOG_ERROR("[{}:{} {}] {}", loc.file_name(), loc.line(), loc.function_name(), msg);
            throw SyscallException(msg, e, loc);
        } else {
            NET_LOG_WARN("[{}:{} {}] {}", loc.file_name(), loc.line(), loc.function_name(), msg);
        }
    }

    // Socket encapsulation
    inline bool set_nonblocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        check_syscall(flags, "fnctl F_GETFL");
        check_syscall(fcntl(fd, F_SETFL, flags | O_NONBLOCK), "fnctl F_SETFL");
        return true;
    }

    inline bool set_reuse_addr(int fd) {
        int opt = 1;
        check_syscall(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)), "setsockopt SO_REUSEADDR");
        return true;
    }

    inline bool set_tcp_nodelay(int fd) {
        int opt = 1;
        check_syscall(setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&opt,sizeof(opt)), "setsocketopt TCP_NODELAY");
        return true;
    }
    inline void close_safe(int fd) {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    inline ssize_t writen(int fd, const void *buf, size_t count) {
        size_t nleft = count;
        const char *ptr = static_cast<const char *>(buf);

        while (nleft > 0) {
            ssize_t nwritten = write(fd, ptr, nleft);
            if (nwritten < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return count - nleft;
                NET_LOG_ERROR("writen failed: {}", strerror(errno));
                return -1;
            }
            nleft -= nwritten;
            ptr += nwritten;
        }
        return count;
    }

    // Epoll encapsulation
    inline int epoll_create() {
        int epfd = epoll_create1(0);
        check_syscall(epfd, "epoll_create1");
        return epfd;
    }

    inline void epoll_add(int epfd, int fd, uint32_t events = EPOLLIN | EPOLLET,
                          std::source_location loc = std::source_location::current()) {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        check_syscall(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev), "epoll_ctl ADD", loc);
    }

    inline void epoll_mod(int epfd, int fd, uint32_t events,
                          std::source_location loc = std::source_location::current()) {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        check_syscall(epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev), "epoll_ctl MOD", loc);
    }

    inline void epoll_del(int epfd, int fd, std::source_location loc = std::source_location::current()) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    }

    // Socket RAII
    struct SocketFd {
        int fd = -1;
        explicit SocketFd(int f) : fd(f) {}
        ~SocketFd() { close_safe(fd); }

        [[nodiscard]] int get() const noexcept { return fd; }
        [[nodiscard]] bool valid() const noexcept { return fd != -1; }

        SocketFd(const SocketFd &) = delete;
        SocketFd &operator=(const SocketFd &) = delete;

        SocketFd(SocketFd&& other) noexcept : fd(other.fd) {other.fd = -1;}

        SocketFd& operator=(SocketFd&& other) noexcept {
            if (this != &other) {
                close_safe(fd);
                fd = other.fd;
                other.fd = -1;
            }
            return *this;
        }
    };

    struct SocketDeleter {
        void operator()(SocketFd *s) const noexcept {
            if (s && s->valid()) {
                ::shutdown(s->get(), SHUT_RDWR);
                close_safe(s->get());
            }
            delete s;
        }
    };

    using SocketPtr = std::unique_ptr<SocketFd, SocketDeleter>;

    inline SocketPtr make_socket_raii(int domain, int type, int protocol) {
        int fd = socket(domain, type, protocol);
        check_syscall(fd, "socket");
        return SocketPtr(new SocketFd(fd), SocketDeleter{});
    }

    // Epoll RAII
    class EpollFd {
    public:
        explicit EpollFd() : fd_(net_utils::epoll_create()) {}
        ~EpollFd() { close_safe(fd_); }

        EpollFd(const EpollFd &) = delete;
        EpollFd &operator=(const EpollFd &) = delete;

        EpollFd(EpollFd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

        EpollFd &operator=(EpollFd &&other) noexcept {
            if (this != &other) {
                close_safe(fd_);
                fd_ = other.fd_;
                other.fd_ = -1;
            }
            return *this;
        }
        int get() const noexcept { return fd_; }
        operator int() const noexcept { return fd_; }

    private:
        int fd_;
    };
} // namespace net_utils
