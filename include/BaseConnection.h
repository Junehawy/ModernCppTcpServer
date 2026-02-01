#pragma once
#include <functional>
#include <netinet/in.h>
#include <shared_mutex>
#include <string>

#include "buffer_utils.h"
#include "config.h"
#include "net_utils.h"

// Abstract base class for TCP connections
// Supporting both line-based and HTTP protocols
class BaseConnection : public std::enable_shared_from_this<BaseConnection> {
public:
    using MessageHandler = std::function<void(BaseConnection *, std::string)>;

    BaseConnection(net_utils::SocketPtr sock, sockaddr_in addr, MessageHandler handler);
    virtual ~BaseConnection();

    // Disable copy/move semantics for safety since connections are managed by shared_ptr
    BaseConnection(const BaseConnection &) = delete;
    BaseConnection &operator=(const BaseConnection &) = delete;
    BaseConnection(BaseConnection &&) = delete;
    BaseConnection &operator=(BaseConnection &&) = delete;

    // Thread-safe send methods with automatic buffering
    ssize_t send(const std::string &msg);
    ssize_t send(const char *data, size_t len);
    ssize_t send(Buffer &buffer);

    // Graceful connection shutdown with linger option
    virtual void shutdown();
    // Trigger async write when buffer becomes non-empty
    virtual void buffer_output() = 0;

    // Connection state and timeout management
    bool is_alive() const { return running_.load(std::memory_order_acquire); }
    void update_active_time() { last_active_ = std::chrono::steady_clock::now(); }
    auto get_last_active() const { return last_active_; }

    std::string get_peer_info() const;
    virtual int get_fd() const = 0;

    // Idle timeout configuration
    void enable_idle_timeout(const bool enable) { enable_idle_timeout_ = enable; }
    void set_idle_timeout(const std::chrono::seconds timeout) { idle_timeout_ = timeout; }
    bool is_idle_timeout_enabled() const { return enable_idle_timeout_; }
    std::chrono::seconds get_idle_timeout() const { return idle_timeout_; }

    // Buffer access for protocol parsing
    Buffer &input_buffer() { return input_buffer_; }
    Buffer &output_buffer() { return output_buffer_; }
    bool has_pending_write() const {
        std::shared_lock lock{buffer_mutex_};
        return output_buffer_.readable_bytes() > 0;
    }

protected:
    net_utils::SocketPtr socket_;
    sockaddr_in client_addr_;
    MessageHandler message_handler_;

    Buffer input_buffer_;
    Buffer output_buffer_;

    mutable std::shared_mutex buffer_mutex_;
    std::atomic<bool> running_{true};
    std::chrono::steady_clock::time_point last_active_;
    std::chrono::seconds idle_timeout_{DEFAULT_IDLE_TIMEOUT};
    bool enable_idle_timeout_{true};
};
