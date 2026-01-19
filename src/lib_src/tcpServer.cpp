#include "../../include/tcpServer.h"
#include <arpa/inet.h>
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

TcpServer::TcpServer(int port, int backlog)
    : server_fd_(nullptr)
    , port_(port)
    , backlog_(backlog)
    , running_(true) {
    // Create listening socket
    server_fd_ = make_socket_raii(AF_INET, SOCK_STREAM, 0);

    // Allow port reuse immediately after release
    if (!set_reuse_addr(server_fd_->get())) {
        throw SocketException("set_reuse_addr failed");
    }

    sockaddr_in server_addr {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port_);

    // Bind to any address:port
    if (bind(server_fd_->get(),
            reinterpret_cast<sockaddr*>(&server_addr),
            sizeof(server_addr))
        == -1) {
        throw SocketException("bind failed");
    }

    // Start listening for incoming connections
    if (listen(server_fd_->get(), backlog_) == -1) {
        throw SocketException("listen failed");
    }

    spdlog::info("Server listening on port {}", port_);
}

TcpServer::~TcpServer() {
    spdlog::info("TcpServer destroyed.");
}

// Start accepting clients in background thread
void TcpServer::start(const ClientHandler& handler) {
    // Register global signal handlers for graceful shutdown
    std::signal(SIGINT, global_signal_handler);
    std::signal(SIGTERM, global_signal_handler);

    // Accept loop runs in dedicated thread
    accept_thread_ = std::jthread([this, handler]() {
        while (running_ && !g_server_should_stop) {
            sockaddr_in client_addr {};
            socklen_t client_len = sizeof(client_addr);

            int client_raw_fd = accept(server_fd_->get(),
                reinterpret_cast<sockaddr*>(&client_addr),
                &client_len);

            if (client_raw_fd == -1) {
                if (errno == EINTR || g_server_should_stop)
                    break;
                spdlog::error("Accept failed: {}", strerror(errno));
                continue;
            }

            // Wrap raw fd with RAII socket pointer
            SocketPtr client_fd { new SocketFd(client_raw_fd), SocketDeleter {} };

            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

            spdlog::info("Client connected: {}:{}",
                client_ip, ntohs(client_addr.sin_port));

            // Hand over to user-provided handler (usually creates Connection)
            handler(std::move(client_fd), client_addr);
        }
    });

    // Block here until shutdown signal is received
    {
        std::unique_lock<std::mutex> lock(g_shutdown_mutex);
        g_shutdown_cv.wait(lock, [] { return g_server_should_stop.load(); });
    }

    spdlog::info("Shutdown signal received, stopping accept loop...");

    running_ = false;

    if (server_fd_) {
        ::shutdown(server_fd_->get(), SHUT_RDWR);
    }
}

// Trigger shutdown from anywhere (thread-safe)
void TcpServer::shutdown() {
    // Only act if we are the first to set the flag
    if (!g_server_should_stop.exchange(true)) {
        // Wake up the waiting start() thread
        {
            std::lock_guard<std::mutex> lock(g_shutdown_mutex);
            g_shutdown_cv.notify_all();
        }

        if (server_fd_ && server_fd_->valid()) {
            ::shutdown(server_fd_->get(), SHUT_RDWR);
        }

        running_ = false;
    }
}