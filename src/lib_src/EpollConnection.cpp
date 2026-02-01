#include "../../include/EpollConnection.h"

EpollConnection::EpollConnection(net_utils::SocketPtr sock, sockaddr_in addr, MessageHandler handler) :
    BaseConnection(std::move(sock), addr, std::move(handler)) {}

void EpollConnection::handle_read() {
    if (!is_alive())
        return;

    char temp[READ_CHUNK_SIZE];
    bool has_new_data = false;

    while (true) {
        // Read data from socket
        ssize_t n = ::read(get_fd(), temp, sizeof(temp));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            NET_LOG_ERROR("{} read error: {}", get_fd(), strerror(errno));
            shutdown();
            return;
        }
        if (n == 0) {
            if (input_buffer_.readable_bytes() == 0) {
                NET_LOG_DEBUG("Peer {} closed connection", get_peer_info());
                shutdown();
                return;
            }
            break;
        }

        has_new_data = true;
        try {
            input_buffer_.append(temp, n);
            if (has_http_handler()) {
                current_raw_buffer_.append(temp, n);
            }
        } catch (const std::exception &e) {
            NET_LOG_ERROR("Buffer overflow for {}: {}", get_peer_info(), e.what());
            shutdown();
            return;
        }
    }

    if (!has_new_data && input_buffer_.readable_bytes() == 0)
        return;

    update_active_time();

    if (!protocol_determined_ && input_buffer_.readable_bytes() > 0) {
        const char* data = input_buffer_.peek();
        size_t len = input_buffer_.readable_bytes();

        size_t check_len = std::min(len,size_t(128));

        std::string_view peek_view(data, check_len);

        static const std::vector<std::string_view> http_starts = {
            "GET ", "POST ", "HEAD ", "PUT ", "DELETE ", "OPTIONS ", "PATCH ", "CONNECT ", "TRACE "
        };

        bool looks_like_http = false;
        for (const auto& prefix : http_starts) {
            if (peek_view.starts_with(prefix)) {
                looks_like_http = true;
                break;
            }
        }

        if (!looks_like_http) {
            std::string_view full_view(data, len);
            size_t pos = full_view.find(" HTTP/");
            if (pos != std::string_view::npos && pos < 100) {
                looks_like_http = true;
            }
        }

        if (looks_like_http) {
            use_http_mode_ = true;
            if (!http_parser_.has_value()) {
                http_parser_.emplace();
            }
        } else {
            use_http_mode_ = false;
            http_handler_ = nullptr;
        }

        protocol_determined_ = true;
    }

    if (use_http_mode_ || has_http_handler()) {
        if (!http_parser_.has_value()) {
            http_parser_.emplace();
        }

        bool should_close = false;

        while (input_buffer_.readable_bytes() > 0 && is_alive()) {
            size_t consumed = http_parser_->parse(input_buffer_.peek(), input_buffer_.readable_bytes());
            input_buffer_.retrieve(consumed);

            if (http_parser_->has_error()) {
                NET_LOG_ERROR("HTTP parse error from {}: {}", get_peer_info(),
                              http_parser_->partial_request().parser_error);

                static const std::string bad_request =
                        "HTTP/1.1 400 Bad Request\r\nContent-Length:0\r\nConnection: close\r\n\r\n";
                send(bad_request);
                shutdown();
                return;
            }

            if (http_parser_->is_complete()) {
                auto req = http_parser_->get_request();

                size_t header_end = current_raw_buffer_.find("\r\n\r\n");
                size_t total_len = (header_end != std::string::npos) ? header_end + 4 + req.body.size()
                                                                     : current_raw_buffer_.size();

                std::string raw_req = current_raw_buffer_.substr(0, total_len);
                current_raw_buffer_.erase(0, total_len);
                pending_requests_.emplace_back(std::move(req), std::move(raw_req));

                pipeline_depth_++;

                if (pipeline_depth_ > MAX_HTTP_PIPELINE) {
                    NET_LOG_WARN("Pipeline depth exceeded from {}", get_peer_info());
                    shutdown();
                    return;
                }

                if (!pending_requests_.back().first.keep_alive) {
                    should_close = true;
                }
            } else {
                break;
            }
        }

        while (!pending_requests_.empty() && is_alive()) {
            auto [req, raw] = std::move(pending_requests_.front());
            pending_requests_.pop_front();
            pipeline_depth_--;

            if (http_handler_) {
                http_handler_(std::static_pointer_cast<EpollConnection>(shared_from_this()), req, raw);
            }
        }

        if (should_close && pending_requests_.empty()) {
            shutdown();
        }
    } else {
        handle_line_protocol();
    }
}

void EpollConnection::handle_line_protocol() {
    while (true) {
        const char *eol = input_buffer_.find_eol();
        if (!eol)
            break;

        std::string line(input_buffer_.peek(), eol - input_buffer_.peek());
        input_buffer_.retrieve_until(eol + 1);

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (!line.empty() && message_handler_) {
            message_handler_(shared_from_this().get(), line);
        }
    }
}

void EpollConnection::handle_write() {
    if (!is_alive() || !socket_ || !socket_->valid())
        return;

    std::lock_guard lock(buffer_mutex_);

    if (output_buffer_.readable_bytes() == 0)
        return;

    ssize_t n = ::write(get_fd(), output_buffer_.peek(), output_buffer_.readable_bytes());
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        if (errno != EINTR) {
            NET_LOG_ERROR("{} write error: {}", get_peer_info(), strerror(errno));
            shutdown();
            return;
        }
        return;
    }

    output_buffer_.retrieve(n);

    if (n > 0 && output_buffer_.readable_bytes() < LOW_WATER_MARK) {
        output_buffer_.shrink_if_needed();
    }
}

void EpollConnection::handle_error() {
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(get_fd(), SOL_SOCKET, SO_ERROR, &error, &len);
    NET_LOG_ERROR("{} connection error, closing", get_peer_info());
    shutdown();
}
