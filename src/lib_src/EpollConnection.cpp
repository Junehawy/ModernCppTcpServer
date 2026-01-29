#include "../../include/EpollConnection.h"

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

    if (has_http_handler()) {
        if (http_state_ == HttpReadState::ReadingHeader) {
            size_t header_end = local_buffer.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string header_part = local_buffer.substr(0, header_end + 4);

                SimpleHttpRequest temp_req = parse_simple_http(header_part);

                pending_header_ = std::move(header_part);

                size_t body_start = header_end + 4;
                size_t remaining = local_buffer.size() - body_start;

                if (temp_req.content_length > 0) {
                    http_state_ = HttpReadState::ReadingBody;
                    expected_body_size_ = temp_req.content_length;
                    pending_body_.clear();

                    if (remaining > 0) {
                        size_t take = std::min(remaining, expected_body_size_);
                        pending_body_.append(local_buffer, body_start, take);
                        local_buffer.erase(0, body_start + take);
                    }

                    if (pending_body_.size() < expected_body_size_) {
                        std::unique_lock lock(buffer_mutex_);
                        read_buffer_ = std::move(local_buffer);
                        return;
                    }
                }

                // body
                SimpleHttpRequest req = parse_simple_http(pending_header_);
                req.body = std::move(pending_body_);

                process_http_request(pending_header_ + req.body);

                http_state_ = HttpReadState::ReadingHeader;
                expected_body_size_ = 0;
                pending_header_.clear();
                pending_body_.clear();

                if (!local_buffer.empty()) {
                    std::unique_lock lock(buffer_mutex_);
                    read_buffer_ = std::move(local_buffer);
                }
                return;
            }
        }
        else if (http_state_ == HttpReadState::ReadingBody) {
            if (!local_buffer.empty()) {
                size_t needed = expected_body_size_ - pending_body_.size();
                size_t take = std::min(local_buffer.size(), needed);

                pending_body_.append(local_buffer, 0, take);
                local_buffer.erase(0, take);

                if (pending_body_.size() >= expected_body_size_) {
                    SimpleHttpRequest req = parse_simple_http(pending_header_);
                    req.body = std::move(pending_body_);

                    process_http_request(pending_header_ + req.body);

                    http_state_ = HttpReadState::ReadingHeader;
                    expected_body_size_ = 0;
                    pending_header_.clear();
                    pending_body_.clear();
                }
            }

            if (!local_buffer.empty()) {
                std::unique_lock lock(buffer_mutex_);
                read_buffer_ = std::move(local_buffer);
            }
            return;
        }
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

void EpollConnection::set_http_handler(HttpMessageHandler handler) {
    http_handler_ = std::move(handler);
}

bool EpollConnection::has_http_handler() const {
    return http_handler_ != nullptr;
}

void EpollConnection::process_http_request(const std::string& raw_request) {
    if (!http_handler_) {
        if (message_handler_) {
            message_handler_(this, raw_request);
        }
        return;
    }

    SimpleHttpRequest parsed = parse_simple_http(raw_request);
    http_handler_(this, raw_request, parsed);
}