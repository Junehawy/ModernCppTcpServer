#include "../../include/BaseConnection.h"

#include <arpa/inet.h>

BaseConnection::BaseConnection(net_utils::SocketPtr sock, const sockaddr_in addr, MessageHandler handler)
    :socket_(std::move(sock)),client_addr_(addr),message_handler_(std::move(handler)),last_active_((std::chrono::steady_clock::now())){  }

BaseConnection::~BaseConnection() { BaseConnection::shutdown(); }

// Shutdown connection,clear buffers
void BaseConnection::shutdown() {
    if (bool expected = true; !running_.compare_exchange_strong(expected,false)) return;

    if (socket_ && socket_->valid()) {
        const int fd = socket_->get();

        constexpr linger ling{1,5};
        net_utils::check_syscall(setsockopt(fd,SOL_SOCKET,SO_LINGER,&ling,sizeof(ling)),"setsockopt SO_LINGER");
        ::shutdown(fd,SHUT_RDWR);
    }
    socket_.reset();

    {
        std::lock_guard lock(buffer_mutex_);
        input_buffer_.retrieve_all();
        output_buffer_.retrieve_all();
    }
}

ssize_t BaseConnection::send(const std::string &msg) {
    return send(msg.data(),msg.size());
}

ssize_t BaseConnection::send(const char *data, size_t len) {
    if (!is_alive() || len == 0) return 0;

    bool become_non_empty = false;
    {
        std::lock_guard lock(buffer_mutex_);
        if (output_buffer_.readable_bytes() + len > MAX_BUFFER_SIZE) {
            NET_LOG_ERROR("Write buffer overflow for {}, closing",get_peer_info());
            shutdown();
            return -1;
        }

        const bool was_empty = output_buffer_.readable_bytes() == 0;
        output_buffer_.append(data, len);
        become_non_empty = was_empty && (output_buffer_.readable_bytes() > 0);
    }

    if (become_non_empty) {
        buffer_output();    // Notify reactor to watch EPOLLOUT
    }

    return len;
}

ssize_t BaseConnection::send(Buffer& buffer) {
    if (!is_alive()) return 0;
    const ssize_t result = send(buffer.peek(),buffer.readable_bytes());
    if (result > 0) {
        buffer.retrieve(result);
    }
    return result;
}

std::string BaseConnection::get_peer_info() const {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr_.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(client_addr_.sin_port));
}

