#pragma once
#include "BaseConnection.h"
class EpollConnection : public BaseConnection {
public:
    EpollConnection(SocketPtr sock,sockaddr_in addr,MessageHandler handler);
    void handle_read();
    void handle_write();
    void handle_error();
    int get_fd() const override {return socket_ ? socket_->get():-1;}
};