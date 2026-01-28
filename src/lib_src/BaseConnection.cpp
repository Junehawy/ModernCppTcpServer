#include "../../include/BaseConnection.h"

BaseConnection::BaseConnection(SocketPtr sock, sockaddr_in addr, MessageHandler handler)
    :socket_(std::move(sock)),client_addr_(addr),message_handler_(std::move(handler)),last_active_((std::chrono::steady_clock::now())){  }

BaseConnection::~BaseConnection(){shutdown(); }

// Shutdown connection,clear buffers
void BaseConnection::shutdown() {
    if (!running_.exchange(false)) return;
    if (socket_ && socket_->valid()) {
        int fd = socket_->get();
        linger ling{1,5};
        setsockopt(fd,SOL_SOCKET,SO_LINGER,&ling,sizeof(ling));
        ::shutdown(fd,SHUT_RDWR);
    }
    socket_.reset();
    {
        std::unique_lock lock(buffer_mutex_);
        read_buffer_.clear();
        write_buffer_.clear();
    }
}

ssize_t BaseConnection::send(const std::string &msg) {
    if (!is_alive()) return -1;
    {
        std::unique_lock lock(buffer_mutex_);
        if (write_buffer_.size() + msg.size() > MAX_BUFFER_SIZE) {
            spdlog::error("Write buffer overflow for {}, closing",get_peer_info());
            shutdown();
            return -1;
        }
        write_buffer_ += msg;
    }
    return static_cast<ssize_t>(msg.size());
}

std::string BaseConnection::get_peer_info() const {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr_.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(client_addr_.sin_port));
}

void BaseConnection::update_active_time() {
    last_active_ = std::chrono::steady_clock::now();
}

bool BaseConnection::is_alive() const {
    return running_.load(std::memory_order_acquire);
}
