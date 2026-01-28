#include "../../include/tcpClient.h"
#include <fcntl.h>
#include <iostream>
#include <poll.h>

#include "spdlog/spdlog.h"

TcpClient::TcpClient(const std::string &server_ip, int port) : sock_fd_(make_socket_raii(AF_INET, SOCK_STREAM, 0)) {
    // Create TCP socket
    if (sock_fd_->get() == -1)
        throw SocketException("socket creation failed");

    int flag = fcntl(sock_fd_->get(), F_GETFL, 0);
    fcntl(sock_fd_->get(), F_SETFL, flag | O_NONBLOCK);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    // Convert IP string to binary format
    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        close_fd(sock_fd_->get());
        throw SocketException("invalid address");
    }

    // Connect to server
    if (connect(sock_fd_->get(), reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)) == -1) {
        if (errno == EINPROGRESS) {
            fd_set writefds;
            FD_ZERO(&writefds);
            FD_SET(sock_fd_->get(), &writefds);

            struct timeval tv{};
            tv.tv_sec = 5;
            tv.tv_usec = 0;

            int sel_ret = select(sock_fd_->get() + 1, nullptr, &writefds, nullptr, &tv);
            if (sel_ret >0 && FD_ISSET(sock_fd_->get(), &writefds)) {
                int error = 0;
                socklen_t len = sizeof(error);
                getsockopt(sock_fd_->get(), SOL_SOCKET, SO_ERROR, &error, &len);

                if (error == 0) {
                    spdlog::info("Client connected");
                    return;
                }else {
                    throw SocketException("socket error");
                }
            }else if (sel_ret == 0) {
                throw SocketException("connection timeout");
            }else {
                throw SocketException("select error");
            }
        }else {
            throw SocketException("connect failed");
        }
    }

    std::cout << "Connected to server " << server_ip << ":" << port << std::endl;
}

// Send a message (appends \n as line terminator)
void TcpClient::send_message(const std::string &msg) {
    std::string data = msg + "\n";

    // Use writen() to ensure all bytes are sent
    if (writen(sock_fd_->get(), data.c_str(), data.size()) == -1) {
        throw SocketException("write failed");
    }
}

// Receive one line from server (until \n)
std::string TcpClient::receive_line() {
    std::string line;
    char buf[1024];

    while (true) {
        struct pollfd pfd{};
        pfd.fd = sock_fd_->get();
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 2000); //2s
        if (ret < 0) {
            if (errno == EINTR) continue;
            throw SocketException("poll failed");
        }
        if (ret == 0) {
            throw SocketException("connection timeout");
        }

        ssize_t n = recv(sock_fd_->get(), buf, sizeof(buf) - 1, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }

            throw SocketException("recv failed");
        }
        if (n == 0)
            throw SocketException("connection closed");

        line.append(buf, n);

        size_t pos;
        if ((pos = line.find('\n')) != std::string::npos) {
            std::string res = line.substr(0, pos);
            line.erase(0, pos + 1); // Keep remaining data for next read
            return res;
        }
    }
}
