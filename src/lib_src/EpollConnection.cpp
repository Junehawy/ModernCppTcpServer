#include "../../include/EpollConnection.h"

#include "SubReactor.h"

EpollConnection::EpollConnection(net_utils::SocketPtr sock, const sockaddr_in addr, MessageHandler handler) :
    BaseConnection(std::move(sock), addr, std::move(handler)) {}

// Fill input buffer and process complete messages
void EpollConnection::handle_read() {
    if (!is_alive())
        return;

    char temp[READ_CHUNK_SIZE];

    try {
        while (true) {
            // Read data from socket
            const ssize_t n = ::read(get_fd(), temp, sizeof(temp));
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
                shutdown();
                return;
            }

            input_buffer_.append(temp, n);

            // Save raw data
            if (has_http_handler()) {
                current_raw_buffer_.append(temp, n);
            }
        }

        update_active_time();

        // Auto-detect protocol based on first bytes
        if (!protocol_determined_ && input_buffer_.readable_bytes() > 0) {
            const char *data = input_buffer_.peek();
            const size_t len = input_buffer_.readable_bytes();

            const size_t check_len = std::min(len, static_cast<size_t>(128));
            std::string_view peek_view(data, check_len);

            static const std::vector<std::string_view> http_starts = {
                    "GET ", "POST ", "HEAD ", "PUT ", "DELETE ", "OPTIONS ", "PATCH ", "CONNECT ", "TRACE "};

            bool looks_like_http = false;
            for (const auto &prefix: http_starts) {
                if (peek_view.starts_with(prefix)) {
                    looks_like_http = true;
                    break;
                }
            }

            // Check if contains "HTTP/"
            if (!looks_like_http) {
                std::string_view full_view(data, len);
                if (const size_t pos = full_view.find(" HTTP/"); pos != std::string_view::npos && pos < 100) {
                    looks_like_http = true;
                }
            }

            if (looks_like_http) {
                use_http_mode_ = true;

                // Create HTTP parser
                if (!http_parser_.has_value()) {
                    http_parser_.emplace();
                }
            } else {
                use_http_mode_ = false;
                http_handler_ = nullptr;
            }

            protocol_determined_ = true;
        }

        // HTTP mode: incremental parse with pipelining support
        if (use_http_mode_ || has_http_handler()) {
            if (!http_parser_.has_value()) {
                http_parser_.emplace();
            }

            bool should_close = false;

            while (input_buffer_.readable_bytes() > 0 && is_alive()) {
                const size_t consumed = http_parser_->parse(input_buffer_.peek(), input_buffer_.readable_bytes());
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
                    try {
                        auto req = http_parser_->get_request();

                        const size_t header_end = current_raw_buffer_.find("\r\n\r\n");
                        const size_t total_len = (header_end != std::string::npos) ? header_end + 4 + req.body.size()
                                                                                   : current_raw_buffer_.size();

                        std::string raw_req = current_raw_buffer_.substr(0, total_len);
                        current_raw_buffer_.erase(0, total_len);

                        // FIFO
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
                    } catch (const std::exception &e) {
                        NET_LOG_ERROR("Request processing failed: {}", e.what());
                        shutdown();
                        return;
                    }
                } else {
                    break;  // Data is imcomplete
                }
            }

            // Process ready requests in order
            while (!pending_requests_.empty() && is_alive()) {
                auto [req, raw] = std::move(pending_requests_.front());
                pending_requests_.pop_front();
                pipeline_depth_--;

                if (http_handler_) {
                    try {
                        // Safely get shared_ptr before calling handler
                        auto self = shared_from_this();
                        auto epoll_self = std::static_pointer_cast<EpollConnection>(self);
                        http_handler_(epoll_self, req, raw);
                    } catch (const std::bad_weak_ptr &e) {
                        NET_LOG_ERROR("Invalid weak_ptr in HTTP handler: {}", e.what());
                        shutdown();
                        return;
                    } catch (const std::exception &e) {
                        NET_LOG_ERROR("HTTP handler exception from {}: {}", get_peer_info(), e.what());
                        // Continue processing - don't kill connection for handler errors
                        static const std::string error_response =
                                "HTTP/1.1 500 Internal Server Error\r\nContent-Length:0\r\nConnection: close\r\n\r\n";
                        send(error_response);
                        should_close = true;
                    }
                }
            }

            if (should_close && pending_requests_.empty()) {
                shutdown();
            }
        } else {
            handle_line_protocol();
        }
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Unhandled exception in handle_read from {}: {}", get_peer_info(), e.what());
        shutdown();
    } catch (...) {
        NET_LOG_ERROR("Unknown exception in handle_read from {}", get_peer_info());
        shutdown();
    }
}

// Split buffer by newlines and invoke handler per line
void EpollConnection::handle_line_protocol() {
    try {
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
                try {
                    // Protected shared_from_this() call
                    auto self = shared_from_this();
                    message_handler_(self.get(), line);
                } catch (const std::bad_weak_ptr &e) {
                    NET_LOG_ERROR("Invalid weak_ptr in message handler: {}", e.what());
                    shutdown();
                    return;
                } catch (const std::exception &e) {
                    NET_LOG_ERROR("Message handler exception from {}: {}", get_peer_info(), e.what());
                }
            }
        }
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Exception in line protocol from {}: {}", get_peer_info(), e.what());
        shutdown();
    }
}

// Flush output buffer to socket (called when EPOLLOUT ready)
void EpollConnection::handle_write() {
    if (!is_alive() || !socket_ || !socket_->valid())
        return;

    try {
        std::lock_guard lock(buffer_mutex_);

        if (output_buffer_.readable_bytes() == 0) {
            if (reactor_) {
                reactor_->disable_writing(get_fd());
            }
            return;
        }

        const ssize_t n = ::write(get_fd(), output_buffer_.peek(), output_buffer_.readable_bytes());
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

        // Stop write if send over
        if (output_buffer_.readable_bytes() == 0 && reactor_) {
            reactor_->disable_writing(get_fd());
        }

        if (n > 0 && output_buffer_.readable_bytes() < LOW_WATER_MARK) {
            output_buffer_.shrink_if_needed();
        }
    } catch (const std::exception &e) {
        NET_LOG_ERROR("Exception in handle_write from {}: {}", get_peer_info(), e.what());
        shutdown();
    }
}

void EpollConnection::handle_error() {
    NET_LOG_ERROR("{} connection error, closing", get_peer_info());
    shutdown();
}

// Notify reactor to listen write if outbufer become not empty
void EpollConnection::buffer_output() {
    if (reactor_) {
        try {
            reactor_->enable_writing(get_fd());
        } catch (const std::exception &e) {
            NET_LOG_ERROR("Failed to enable writing: {}", e.what());
        }
    }
}
