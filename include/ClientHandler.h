#pragma once
#include <memory>

#include "BaseConnection.h"

class BaseConnection;
using ClientHandler = std::function<std::shared_ptr<BaseConnection>(SocketPtr client_fd,const sockaddr_in& client_addr)>;