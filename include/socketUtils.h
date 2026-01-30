#pragma once
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "spdlog/spdlog.h"

// Base exception class with location,error code and formatted message
class BaseException : public std::exception {
public:
    BaseException(std::string msg, int err_code = 0,
        std::source_location loc = std::source_location::current())
        : err_msg_(std::move(msg))
        , err_code_(err_code)
        , loc_(loc) { }

    // Override what() to return formatted error message
    const char* what() const noexcept override {
        if (formatted_msg_.empty()) {
            format_msg();
        }
        return formatted_msg_.c_str();
    }

    int error_code() const noexcept { return err_code_; }
    const std::source_location& location() const noexcept { return loc_; }

protected:
    mutable std::string formatted_msg_;
    std::string err_msg_;
    int err_code_;
    std::source_location loc_;

private:
    void format_msg() const {
#if __cplusplus >= 202002L
        formatted_msg_ = std::format("[{}:{} {}] {} (errno: {},desc: {})",
            loc_.file_name(),
            loc_.line(),
            loc_.function_name(),
            err_msg_,
            err_code_,
            (err_code_ == 0 ? "N/A" : strerror(err_code_)));

#else
        if (formatted_msg_.empty()) {
            formatted_msg_ = "[" + std::string(loc_.file_name()) + ":" + std::to_string(loc_.line()) + " " + loc_.function_name() + "]" + err_msg_ + " (errno: " + std::to_string(err_code_) + ", desc: " + strerror(err_code_) + ")";
        }
#endif
    }
};

// Socket-specific exception class
class SocketException : public BaseException {
public:
    using BaseException::BaseException;
    SocketException(const std::string msg, int err_code = errno,
        std::source_location loc = std::source_location::current())
        : BaseException("Socket Error: " + std::move(msg), err_code, loc) { }
};


bool set_reuse_addr(int fd);

ssize_t writen(int fd, const void* data, size_t len);

std::string read_line(int fd);

void close_fd(int fd);

// Socket file descriptor
struct SocketFd {
    int fd;
    explicit SocketFd(int f)
        : fd(f) { }
    [[nodiscard]] int get() const noexcept { return fd; }
    [[nodiscard]] bool valid() const noexcept { return fd != -1; }
};

// Deleter for SocketFd unique_ptr
struct SocketDeleter {
    void operator()(SocketFd* s) const noexcept {
        if (s && s->valid()) {
            close_fd(s->fd);
            s->fd = -1; // Mark fd as invalid after close
        }
        delete s;
    }
};

using SocketPtr = std::unique_ptr<SocketFd, SocketDeleter>;

// Create socket with unique_ptr
inline SocketPtr make_socket_raii(int domain, int type, int protocol) {
    int fd = socket(domain, type, protocol);
    if (fd == -1) {
        throw SocketException("socket create failed",errno);
    }
    return SocketPtr(new SocketFd(fd), SocketDeleter {});
}

namespace socket_utils {
    void add_epoll(int epfd,int fd,uint32_t events = EPOLLIN | EPOLLET);
    void mod_epoll(int epfd,int fd,uint32_t events);
    void del_epoll(int epfd,int fd);

    void check_syscall(int ret,const std::string& msg,bool throw_on_fail = true);
}
