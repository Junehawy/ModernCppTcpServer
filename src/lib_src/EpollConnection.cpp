#include "../../include/EpollConnection.h"

#include "../../backup/Connection.h"

EpollConnection::EpollConnection(SocketPtr sock, sockaddr_in addr, MessageHandler handler)
    :BaseConnection(std::move(sock),addr, std::move(handler)){  }

void EpollConnection::handle_read() {
    char temp[READ_CHUNK_SIZE];
    bool has_new_data = false;

    while (true) {
        // Read data from socket
        ssize_t n = ::read(get_fd(),temp,sizeof(temp));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            spdlog::error("{} read error: {}", get_fd(),strerror(errno));
            shutdown();
            return;
        }
        if (n == 0) {
            spdlog::debug("{} read EOF", get_peer_info());
            shutdown();
            return;
        }
        {
            std::unique_lock lock(buffer_mutex_);
            if (read_buffer_.size() + n > MAX_BUFFER_SIZE) {
                spdlog::error("read buffer overflow for {}, closing", get_peer_info());
                shutdown();
                return;
            }
            read_buffer_.append(temp,n);
            has_new_data = true;
        }
        update_active_time();
    }

    if (!has_new_data) return;

    std::string local_buffer;
    {
        std::unique_lock lock(buffer_mutex_);
        local_buffer = std::move(read_buffer_);
        read_buffer_.clear();
    }
    size_t pos;
    while ((pos = local_buffer.find('\n')) != std::string::npos) {
        std::string line = local_buffer.substr(0, pos);
        local_buffer.erase(0, pos+1);
        if (line.empty()) continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (message_handler_) {
            message_handler_(this,line);
            handle_write();
        }
    }
    // Save remaining data back to read buffer
    if (!local_buffer.empty()) {
        std::unique_lock lock(buffer_mutex_);
        read_buffer_ = std::move(local_buffer);
    }
}

void EpollConnection::handle_write() {
    std::string local_buffer;
    {
        std::unique_lock lock(buffer_mutex_);
        local_buffer = std::move(write_buffer_);
        write_buffer_.clear();
    }
    size_t offset = 0;
    // Write all data in local buffer
    while (offset < local_buffer.size()) {
        ssize_t n = ::write(get_fd(),local_buffer.data() + offset,local_buffer.size() - offset);
        if (n<0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            spdlog::error("{} write error: {}", get_fd(),strerror(errno));
            shutdown();
            return;
        }
        offset += n;
    }
    // Save unsent data back to write buffer
    if (offset < local_buffer.size()) {
        std::unique_lock lock(buffer_mutex_);
        write_buffer_ = local_buffer.substr(offset);
    }
}

void EpollConnection::handle_error() {
    spdlog::error("{} connection error, closing", get_peer_info());
    shutdown();
}