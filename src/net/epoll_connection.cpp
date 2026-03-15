#include "../../include/net/epoll_connection.h"

#include "../../include/net/sub_reactor.h"

EpollConnection::EpollConnection(net_utils::SocketPtr sock, const sockaddr_in addr, MessageHandler handler) :
    BaseConnection(std::move(sock), addr, std::move(handler)) {}

// Fill input buffer and process complete messages
void EpollConnection::handle_read() {
    if (!is_alive())
        return;

    NET_LOG_DEBUG("handle_read called, fd={}, buffer_bytes={}", get_fd(), input_buffer_.readable_bytes());

    try {
        int saved_errno = 0;
        const ssize_t total_read = input_buffer_.read_fd(get_fd(), &saved_errno);
        NET_LOG_DEBUG("read_fd returned={}, saved_errno={}, buffer_bytes_after={}", total_read, saved_errno,
                      input_buffer_.readable_bytes());

        if (total_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            NET_LOG_ERROR("{} read error: {}", get_fd(), strerror(errno));
            shutdown();
            return;
        }
        if (total_read == 0) {
            shutdown();
            return;
        }

        update_active_time();
        protocol_detected();

        NET_LOG_DEBUG("dispatch: http={} proto={} has_handler={}", use_http_mode_, use_protobuf_mode_,
                      has_http_handler());

        // HTTP mode: incremental parse with pipelining support
        if (use_http_mode_ || has_http_handler()) {
            if (!http_parser_.has_value()) {
                http_parser_.emplace();
            }

            bool should_close = false;

            while (input_buffer_.readable_bytes() > 0 && is_alive()) {
                const auto data = input_buffer_.peek();
                const size_t avail = input_buffer_.readable_bytes();

                const auto result = http_parser_->parse(data, avail);

                if (result.has_error) {
                    NET_LOG_ERROR("HTTP parse error from {}: {}", get_peer_info(),
                                  http_parser_->partial_request().parser_error);

                    static const std::string bad_request =
                            "HTTP/1.1 400 Bad Request\r\nContent-Length:0\r\nConnection: close\r\n\r\n";
                    send(bad_request);
                    shutdown();
                    return;
                }

                if (result.is_complete) {
                    try {
                        auto req = http_parser_->get_request();

                        std::string_view raw_view(data + result.request_start_index, result.request_total_len);

                        // FIFO
                        pending_requests_.emplace_back(std::move(req), raw_view);

                        pipeline_depth_++;

                        if (pipeline_depth_ > MAX_HTTP_PIPELINE) {
                            NET_LOG_WARN("Pipeline depth exceeded from {}", get_peer_info());
                            shutdown();
                            return;
                        }

                        if (!pending_requests_.back().request.keep_alive) {
                            should_close = true;
                        }
                    } catch (const std::exception &e) {
                        NET_LOG_ERROR("Request processing failed: {}", e.what());
                        shutdown();
                        return;
                    }
                } else {
                    break; // Data is imcomplete
                }

                input_buffer_.retrieve(result.consumed_bytes);
            }

            const bool using_worker_pool = (worker_pool_ != nullptr);

            // Process ready requests in order
            while (!pending_requests_.empty() && is_alive()) {
                auto [req, raw] = std::move(pending_requests_.front());
                pending_requests_.pop_front();
                pipeline_depth_--;

                if (http_handler_) {
                    try {
                        // Safely get shared_ptr before calling handler
                        auto self = shared_from_this();
                        const auto epoll_self = std::static_pointer_cast<EpollConnection>(self);

                        if (using_worker_pool) {
                            std::string raw_copy{raw.data(), raw.size()};
                            const bool close_after = !req.keep_alive;

                            worker_pool_->submit([epoll_self, req = std::move(req), raw_copy = std::move(raw_copy),
                                                  close_after]() mutable {
                                if (!epoll_self->is_alive())
                                    return;

                                try {
                                    epoll_self->http_handler_(epoll_self, req, raw_copy);
                                } catch (const std::exception &e) {
                                    NET_LOG_ERROR("Worker HTTP handler exception from {}: {}",
                                                  epoll_self->get_peer_info(), e.what());
                                    static const std::string err = "HTTP/1.1 500 Internal Server Error\r\n"
                                                                   "Content-Length:0\r\nConnection: close\r\n\r\n";
                                    epoll_self->send(err);
                                    epoll_self->shutdown();
                                    return;
                                }
                                if (close_after) {
                                    epoll_self->shutdown();
                                }
                            });
                        } else {
                            try {
                                http_handler_(epoll_self, req, raw.data());
                            } catch (const std::exception &e) {
                                NET_LOG_ERROR("HTTP handler exception from {}: {}", get_peer_info(), e.what());
                                // Continue processing - don't kill connection for handler errors
                                static const std::string error_response =
                                        "HTTP/1.1 500 Internal Server Error\r\nContent-Length:0\r\nConnection: "
                                        "close\r\n\r\n";
                                send(error_response);
                                should_close = true;
                            }
                        }

                    } catch (const std::bad_weak_ptr &e) {
                        NET_LOG_ERROR("Invalid weak_ptr in HTTP handler: {}", e.what());
                        shutdown();
                        return;
                    }

                    if (!using_worker_pool && should_close && pending_requests_.empty()) {
                        shutdown();
                    }
                }
            }
        } else if (use_protobuf_mode_) {
            handle_protobuf();
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
    NET_LOG_DEBUG("line_protocol: readable={}, msg_handler={}", input_buffer_.readable_bytes(),
                  message_handler_ ? "valid" : "NULL");
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

void EpollConnection::protocol_detected() {
    if (!protocol_determined_ && input_buffer_.readable_bytes() > 0) {
        const char *data = input_buffer_.peek();
        const size_t len = input_buffer_.readable_bytes();

        const size_t check_len = std::min(len, static_cast<size_t>(128));
        const std::string_view peek_view(data, check_len);

        static const std::vector<std::string_view> http_starts = {"GET ",     "POST ",  "HEAD ",    "PUT ",  "DELETE ",
                                                                  "OPTIONS ", "PATCH ", "CONNECT ", "TRACE "};

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
            use_protobuf_mode_ = false;
            protocol_determined_ = true;

            if (!http_parser_.has_value()) {
                http_parser_.emplace();
            }
            return;
        }

        use_http_mode_ = false;
        http_handler_ = nullptr;

        const bool has_newline = (memchr(data, '\n', len) != nullptr);
        bool looks_text = has_newline;
        if (looks_text) {
            for (size_t i = 0; i < std::min(len, static_cast<size_t>(32)); ++i) {
                if (const auto c = static_cast<unsigned char>(data[i]); c < 32 && c != '\r' && c != '\n' && c != '\t') {
                    looks_text = false;
                    break;
                }
            }
        }

        if (looks_text) {
            protocol_determined_ = true;
            use_protobuf_mode_ = false;
            NET_LOG_INFO("{} using line protocol", get_peer_info());
            return;
        }

        use_protobuf_mode_ = true;
        protocol_determined_ = true;
    }
}

void EpollConnection::handle_protobuf() {
    while (input_buffer_.readable_bytes() >= 4 && is_alive()) {
        if (!length_read_) {
            uint32_t net_len;
            std::memcpy(&net_len, input_buffer_.peek(), 4);
            expected_length_ = be32toh(net_len);
            input_buffer_.retrieve(4);
            length_read_ = true;

            if (expected_length_ > 4 * 1024 * 1024) {
                NET_LOG_ERROR("Protocol buffer overflow length: {} bytes", expected_length_);
                shutdown();
                return;
            }
        }

        if (input_buffer_.readable_bytes() >= expected_length_) {
            std::string pb_data(input_buffer_.peek(), expected_length_);
            input_buffer_.retrieve(expected_length_);

            moderncpp::Request req;
            if (!req.ParseFromString(pb_data)) {
                NET_LOG_ERROR("Protobuf parse failed from {}", get_peer_info());
                send_protobuf_error(400, "Invalid Protobuf");
                length_read_ = false;
                continue;
            }
            handle_protobuf_request(req);
            length_read_ = false;
        } else {
            break;
        }
    }
}

void EpollConnection::handle_protobuf_request(const moderncpp::Request &req) {
    moderncpp::Response resp;
    resp.set_code(200);
    resp.set_message("OK");

    std::string method = req.method();
    NET_LOG_INFO("{} Protobuf request: method='{}', body_size={}", get_peer_info(), method, req.body().size());

    if (method == "PING") {
        resp.set_message("PONG");
    } else if (method == "ECHO") {
        resp.set_data(req.body());
        resp.set_message("Echo: " + req.body());
    } else {
        resp.set_code(404);
        resp.set_message("Unknown method: " + method);
    }

    send_protobuf(resp);
}

void EpollConnection::send_protobuf(const moderncpp::Response &resp_in) {
    moderncpp::Response resp = resp_in;

    if (resp.code() == 0)
        resp.set_code(200);
    if (resp.message().empty())
        resp.set_message("OK");

    std::string serialized;
    if (!resp.SerializeToString(&serialized)) {
        NET_LOG_ERROR("Failed to serialize Response for {}", get_peer_info());
        return;
    }

    if (serialized.empty()) {
        NET_LOG_WARN("Empty serialized Response, sending dummy");
        serialized = "empty";
    }

    const uint32_t len = htobe32(static_cast<uint32_t>(serialized.size()));
    const std::string prefix(reinterpret_cast<const char *>(&len), 4);

    NET_LOG_DEBUG("Sending Protobuf response: len={}, code={}, msg={}", serialized.size(), resp.code(), resp.message());

    send(prefix + serialized);
}

void EpollConnection::send_protobuf_error(const int code, const std::string &msg) {
    moderncpp::Response resp;
    resp.set_code(code);
    resp.set_message(msg);
    send_protobuf(resp);
}
