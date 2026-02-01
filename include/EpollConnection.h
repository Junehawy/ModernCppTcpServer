#pragma once
#include <deque>

#include "BaseConnection.h"
#include "http_types.h"

class EpollConnection : public BaseConnection {
public:
    using HttpHandler = std::function<void(std::shared_ptr<EpollConnection>,const SimpleHttpRequest&,const std::string&)>;

    EpollConnection(net_utils::SocketPtr sock,sockaddr_in addr,MessageHandler handler);

    void handle_read();
    void handle_write();
    void handle_error();

    int get_fd() const override {return socket_ ? socket_->get():-1;}

    void set_http_handler(HttpHandler handler) {http_handler_ = std::move(handler);}
    bool has_http_handler() const {return static_cast<bool>(http_handler_);}
    void try_flush_output() override {
        handle_write();
    }

private:
    HttpHandler http_handler_;
    std::optional<HttpParser> http_parser_;
    std::string current_raw_buffer_;

    std::deque<std::pair<SimpleHttpRequest,std::string>> pending_requests_;
    size_t pipeline_depth_ = 0;

    bool protocol_determined_ = false;
    bool use_http_mode_ = false;

    void handle_line_protocol();


};