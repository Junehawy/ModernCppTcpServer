#pragma once
#include <memory>

#include "BaseConnection.h"

class BaseConnection;
// Factory function: creates connection objects from accepted sockets
using ClientHandler =
        std::function<std::shared_ptr<BaseConnection>(net_utils::SocketPtr client_fd, const sockaddr_in &client_addr)>;
