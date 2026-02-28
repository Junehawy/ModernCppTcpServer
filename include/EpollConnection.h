#pragma once
#include <deque>

#include "BaseConnection.h"
#include "http_types.h"
#include "message.pb.h"

class SubReactor;

// Concrete connection using epoll for scalable I/O multiplexing
class EpollConnection : public BaseConnection {
public:
    using HttpHandler =
            std::function<void(std::shared_ptr<EpollConnection>, const SimpleHttpRequest &, const std::string &)>;

    EpollConnection(net_utils::SocketPtr sock, sockaddr_in addr, MessageHandler handler);

    // Event handlers called by SubReactor on epoll events
    void handle_read();
    void handle_write();
    void handle_error();

    int get_fd() const override { return socket_ ? socket_->get() : -1; }

    void set_http_handler(HttpHandler handler) { http_handler_ = std::move(handler); }
    bool has_http_handler() const { return static_cast<bool>(http_handler_); }

    void attch_reactor(SubReactor *reactor) { reactor_ = reactor; }
    void buffer_output() override;

private:
    HttpHandler http_handler_;                  // HTTP callback
    std::optional<HttpParser> http_parser_;     // Stateful HTTP parser
    std::string current_raw_buffer_;

    std::deque<std::pair<SimpleHttpRequest, std::string>> pending_requests_;
    size_t pipeline_depth_ = 0;

    bool protocol_determined_ = false;          // Auto-detect HTTP vs line
    bool use_http_mode_ = false;
    SubReactor *reactor_ = nullptr;

    bool use_protobuf_mode_ = false;
    bool length_read_ = false;
    uint32_t expected_length_ = 0;
    std::string current_pb_buffer_;

    void handle_protobuf();
    void handle_protobuf_request(const moderncpp::Request &req);
    void send_protobuf(const moderncpp::Response& resp_in);
    void send_protobuf_error(int code, const std::string &msg);

    void handle_line_protocol();                // Process \n delimited messages
    void protocol_detected();
};
