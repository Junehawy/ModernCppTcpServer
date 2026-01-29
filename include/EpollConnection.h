#pragma once
#include "BaseConnection.h"
#include "http_types.h"
class EpollConnection : public BaseConnection {
public:
    EpollConnection(SocketPtr sock,sockaddr_in addr,MessageHandler handler);
    void handle_read();
    void handle_write();
    void handle_error();

    int get_fd() const override {return socket_ ? socket_->get():-1;}

    using HttpMessageHandler = std::function<void(BaseConnection*, const std::string&, const SimpleHttpRequest&)>;
    void set_http_handler(HttpMessageHandler handler);
    bool has_http_handler() const;
private:
    void process_http_request(const std::string&);

    enum class HttpReadState {
        ReadingHeader,
        ReadingBody
    };

    HttpReadState http_state_ = HttpReadState::ReadingHeader;
    size_t expected_body_size_ = 0;
    std::string pending_header_;
    std::string pending_body_;

    HttpMessageHandler http_handler_ = nullptr;

};