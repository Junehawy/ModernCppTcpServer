#include "../../include/tcpClient.h"
#include <iostream>

TcpClient::TcpClient(const std::string& server_ip, int port)
    : sock_fd_(make_socket_raii(AF_INET, SOCK_STREAM, 0)) {
    // Create TCP socket
    if (sock_fd_->get() == -1)
        throw SocketException("socket creation failed");

    sockaddr_in server_addr {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    // Convert IP string to binary format
    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        close_fd(sock_fd_->get());
        throw SocketException("invalid address");
    }

    // Connect to server
    if (connect(sock_fd_->get(),
            reinterpret_cast<sockaddr*>(&server_addr),
            sizeof(server_addr))
        == -1) {
        close_fd(sock_fd_->get());
        throw SocketException("connect failed");
    }

    std::cout << "Connected to server " << server_ip << ":" << port << std::endl;
}

// Send a message (appends \n as line terminator)
void TcpClient::send_message(const std::string& msg) {
    std::string data = msg + "\n";

    // Use writen() to ensure all bytes are sent
    if (writen(sock_fd_->get(), data.c_str(), data.size()) == -1) {
        throw SocketException("write failed");
    }
}

// Receive one line from server (until \n)
std::string TcpClient::receive_line() {
    std::string line = read_line(sock_fd_->get());

    // Treat empty line as connection closed by server
    if (line.empty()) {
        throw SocketException("Server closed connection");
    }

    return line;
}