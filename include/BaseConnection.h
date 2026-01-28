#pragma once
#include <functional>
#include <shared_mutex>
#include <string>

#include "config.h"
#include "socketUtils.h"

class BaseConnection {
public:
    using MessageHandler = std::function<void(BaseConnection*,std::string)>;
    BaseConnection(SocketPtr sock,sockaddr_in addr,MessageHandler handler);
    virtual ~BaseConnection();

    virtual void shutdown();
    ssize_t send(const std::string& msg);
    std::string get_peer_info() const;
    void update_active_time();
    bool is_alive() const;
    virtual int get_fd() const = 0;

    void enable_idle_timeout(bool enable) { enable_idle_timeout_ = enable; }
    void set_idle_timeout(std::chrono::seconds timeout) { idle_timeout_ = timeout; }

    bool is_idle_timeout_enabled() const { return enable_idle_timeout_; }
    std::chrono::seconds get_idle_timeout() const { return idle_timeout_; }

    SocketPtr socket_;
    sockaddr_in client_addr_;
    MessageHandler message_handler_;
    std::string read_buffer_;
    std::string write_buffer_;
    mutable std::shared_mutex buffer_mutex_;
    std::atomic<bool> running_{true};
    std::chrono::steady_clock::time_point last_active_;
    std::chrono::seconds idle_timeout_{DEFAULT_IDLE_TIMEOUT};
    bool enable_idle_timeout_{true};

    bool has_pending_write() const {
        std::shared_lock lock(buffer_mutex_);
        return !write_buffer_.empty();
    }
};