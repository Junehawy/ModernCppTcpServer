#include "../../include/BaseConnection.h"

#include <arpa/inet.h>

BaseConnection::BaseConnection(net_utils::SocketPtr sock, const sockaddr_in addr, MessageHandler handler) :
    socket_(std::move(sock)), client_addr_(addr), message_handler_(std::move(handler)),
    last_active_((std::chrono::steady_clock::now())) {
    if (!socket_ || !socket_->valid()) {
        throw std::invalid_argument("Invalid socket passed");
    }
}

BaseConnection::~BaseConnection() {
    try {
        BaseConnection::shutdown();
    } catch (const std::exception &e) {
        NET_LOG_ERROR("BaseConnection::~BaseConnection(): {}", e.what());
    } catch (...) {
        NET_LOG_ERROR("BaseConnection::~BaseConnection(): unknown exception");
    }
}

// Shutdown connection,clear buffers
void BaseConnection::shutdown() {
    // Confirm close once
    if (bool expected = true; !running_.compare_exchange_strong(expected, false))
        return;

    try {
        if (socket_ && socket_->valid()) {
            const int fd = socket_->get();

            // Wait 5s to send msg
            constexpr linger ling{1, 5};
            net_utils::check_syscall(setsockopt(fd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling)),
                                     "setsockopt SO_LINGER");
            ::shutdown(fd, SHUT_RDWR);
        }
        socket_.reset();

        // Clear IO buffer
        {
            std::lock_guard lock(buffer_mutex_);
            try {
                input_buffer_.retrieve_all();
                output_buffer_.retrieve_all();
            } catch (const std::exception &e) {
                NET_LOG_ERROR("Exception clearing buffers: {}", e.what());
            }
        }
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Exception in BaseConnection::shutdown(): {}", e.what());
    }
}

ssize_t BaseConnection::send(const std::string &msg) {
    try {
        return send(msg.data(), msg.size());
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Exception in send(string): {}", e.what());
        return -1;
    }
}

ssize_t BaseConnection::send(const char *data, size_t len) {
    if (!is_alive() || len == 0)
        return 0;

    if (!data) {
        NET_LOG_ERROR("Null data pointer in send()");
        return -1;
    }

    try {
        bool become_non_empty = false;

        {
            std::lock_guard lock(buffer_mutex_);
            if (output_buffer_.readable_bytes() + len > MAX_BUFFER_SIZE) {
                NET_LOG_ERROR("Write buffer overflow for {}, closing", get_peer_info());
                shutdown();
                return -1;
            }

            const bool was_empty = output_buffer_.readable_bytes() == 0;
            output_buffer_.append(data, len);
            become_non_empty = was_empty && (output_buffer_.readable_bytes() > 0);
        }

        if (become_non_empty) {
            try {
                buffer_output(); // Notify reactor to watch EPOLLOUT
            } catch (const std::exception &e) {
                NET_LOG_ERROR("Failed to notify reactor: {}", e.what());
            }
        }

        return static_cast<ssize_t>(len);
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Exception in send(): {}", e.what());
        shutdown();
        return -1;
    }
}

ssize_t BaseConnection::send(Buffer &buffer) {
    if (!is_alive())
        return 0;

    try {
        const ssize_t result = send(buffer.peek(), buffer.readable_bytes());
        if (result > 0) {
            buffer.retrieve(result);
        }
        return result;
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Exception in send(Buffer): {}", e.what());
        return -1;
    }
}

std::string BaseConnection::get_peer_info() const {
    try {
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &client_addr_.sin_addr, ip, sizeof(ip)) == nullptr) {
            return "unknown:0";
        }
        return std::string(ip) + ":" + std::to_string(ntohs(client_addr_.sin_port));
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Exception getting peer info: {}", e.what());
        return "error:0";
    }
}
