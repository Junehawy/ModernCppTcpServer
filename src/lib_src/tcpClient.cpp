#include "../../include/tcpClient.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>

#include "spdlog/spdlog.h"

TcpClient::TcpClient(const std::string &server_ip, int port) :server_ip_(server_ip),port_(port) {
    ensure_connection();
}

void TcpClient::ensure_connection() {
    sock_fd_ = net_utils::make_socket_raii(AF_INET, SOCK_STREAM, 0);

    int flag = fcntl(sock_fd_->get(), F_GETFL, 0);
    fcntl(sock_fd_->get(), F_SETFL, flag | O_NONBLOCK);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_);

    // Convert IP string to binary format
    if (inet_pton(AF_INET, server_ip_.c_str(), &server_addr.sin_addr) <= 0) {
        net_utils::close_safe(sock_fd_->get());
        throw net_utils::SyscallException("invalid address");
    }

    int ret = connect(sock_fd_->get(), reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr));
    if (ret < 0 && errno != EINPROGRESS) {
        throw net_utils::SyscallException("connect failed");
    }

    if (ret < 0) {
        pollfd pfd{sock_fd_->get(), POLLOUT, 0};
        ret = poll(&pfd, 1, 5000);
        if (ret <= 0) {
            throw net_utils::SyscallException("poll failed");
        }

        int so_error;
        socklen_t len = sizeof(so_error);
        getsockopt(sock_fd_->get(),SOL_SOCKET,SO_ERROR,&so_error,&len);
        if (so_error != 0) {
            throw net_utils::SyscallException("socket error");
        }
    }

    fcntl(sock_fd_->get(), F_SETFL, flag);
    connected_ = true;
    last_active_time_ = std::chrono::steady_clock::now();
    NET_LOG_INFO("Connected to {}:{}",server_ip_,port_);

}

// Send a message (appends \n as line terminator)
void TcpClient::send_message(const std::string &msg) {
    if (!connected_) {
        throw std::runtime_error("send message not connected");
    }
    std::string data = msg + "\n";
    ssize_t sent = net_utils::writen(sock_fd_->get(), data.data(), data.size());

    // Use writen() to ensure all bytes are sent
    if (sent < 0 || static_cast<size_t>(sent) != data.size()) {
        connected_ = false;
        throw net_utils::SyscallException("Send failed");
    }
    last_active_time_ = std::chrono::steady_clock::now();
}

// Receive one line from server (until \n)
std::string TcpClient::receive_line() {
    if (!connected_) {
        throw std::runtime_error("Not connected");
    }

    while (true) {
        const char* eol = recv_buf_.find_eol();
        if (eol) {
            std::string line(recv_buf_.peek(),eol-recv_buf_.peek());
            recv_buf_.retrieve_until(eol+1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            last_active_time_ = std::chrono::steady_clock::now();
            return line;
        }

        char temp[4096];
        ssize_t n = recv(sock_fd_->get(), temp, sizeof(temp), 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            connected_ = false;
            throw net_utils::SyscallException("recv failed");
        }
        if (n == 0) {
            connected_ = false;
            throw net_utils::SyscallException("connection closed");
        }

        recv_buf_.append(temp, n);

        if (recv_buf_.readable_bytes() > MAX_BUFFER_SIZE) {
            throw std::runtime_error("Receive buffer overflow");
        }
    }
}

std::string TcpClient::receive(size_t max_len) {
    if (!connected_) {
        throw std::runtime_error("Not connected");
    }

    if (recv_buf_.readable_bytes() > 0) {
        size_t to_read = std::min(max_len, recv_buf_.readable_bytes());
        return recv_buf_.retrieve_as_string(to_read);
    }

    std::string result;
    result.resize(max_len);
    ssize_t n = recv(sock_fd_->get(), result.data(), max_len, 0);
    if (n < 0) {
        if (errno == EINTR) return receive(max_len);
        connected_ = false;
        throw net_utils::SyscallException("recv failed");
    }
    if (n == 0) {
        connected_ = false;
        throw net_utils::SyscallException("connection closed");
    }
    result.resize(n);
    last_active_time_ = std::chrono::steady_clock::now();
    return result;
}
