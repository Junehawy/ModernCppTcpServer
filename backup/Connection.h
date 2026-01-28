#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include "../include/socketUtils.h"

class Connection {
public:
    // Callback type for processing received messages
    using MessageHandler = std::function<void(Connection*, std::string)>;

    Connection(SocketPtr sock, sockaddr_in addr, MessageHandler handler);
    ~Connection();

    void start();
    void shutdown();
    [[nodiscard]] bool is_alive() const;
    ssize_t send(const std::string& msg);
    [[nodiscard]] std::string get_peer_info() const;

    // Set idle timeout duration
    void set_idle_timeout(std::chrono::seconds timeout) {
        idle_timeout_ = timeout;
    }

    void enable_idle_timeout(bool enable) { enable_idle_timeout_ = enable; }

    void update_active_time();

    [[nodiscard]] int get_fd() const;

    void handle_read();
    void handle_write();
    void handle_error();

    [[nodiscard]] bool has_pending_write() const {
        return !write_buffer_.empty();
    }

private:
    SocketPtr socket_;
    sockaddr_in client_addr_;
    MessageHandler message_handler_;

    std::jthread worker_;
    std::atomic<bool> running_ { true };

    std::string read_buffer_;
    std::string write_buffer_;
    static constexpr size_t READ_CHUNK_SIZE = 8192;
    // Core worker loop
    void run();

    bool enable_idle_timeout_ { true };
    std::chrono::seconds idle_timeout_ { std::chrono::seconds(60) };
    std::chrono::steady_clock::time_point last_active_ {
        std::chrono::steady_clock::now()
    };
};