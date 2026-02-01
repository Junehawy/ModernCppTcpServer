#include <iostream>
#include <mutex>
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink-inl.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <thread>

#include "../include/stringUtils.h"
#include "../include/tcpServer.h"
#include "http_types.h"

int main() {
    try {
        // Initialize async spdlog with console + rotating file
        spdlog::init_thread_pool(8192, 1);

        const auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::info);
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");

        const auto file_sink =
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>("logs/server.log", 10 * 1024 * 1024, 5);
        file_sink->set_level(spdlog::level::off);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");

        std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};

        const auto logger = std::make_shared<spdlog::async_logger>(
                "server", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::block);

        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::info);

        spdlog::info("spdlog initialized");
    } catch (const spdlog::spdlog_ex &ex) {
        std::cerr << "spdlog init failed: " << ex.what() << std::endl;
        return 1;
    }

    try {
        constexpr bool use_epoll = true;
        TcpServer server(9999, 1024, use_epoll, 8);

        // Start server and define client connection handler
        server.start([&](net_utils::SocketPtr client_fd,
                         const sockaddr_in &client_addr) -> std::shared_ptr<BaseConnection> {
            auto conn = std::make_shared<EpollConnection>(std::move(client_fd), client_addr,
                                                          [&](BaseConnection *self, std::string msg) {
                                                              spdlog::info("{} -> {}", self->get_peer_info(), msg);
                                                              const std::string response = rtrim_cc(msg) + "\n";
                                                              self->send(response);
                                                          });

            conn->set_http_handler([](const std::shared_ptr<EpollConnection> &self, const SimpleHttpRequest &req,
                                      const std::string &raw_request) {
                spdlog::info("[HTTP] {} {} from {}", req.method, req.path, self->get_peer_info());

                std::string body;
                std::string content_type = "text/plain";

                if (req.path == "/" || req.path == "/hello") {
                    body = "Hello from tiny HTTP server!\n";
                } else if (req.path == "/json") {
                    body = R"({"status":"ok","message":"welcome to my server"})";
                    content_type = "application/json";
                } else if (req.path == "/echo") {
                    body = "You sent:\n" + raw_request.substr(0, 500);
                    content_type = "text/plain";
                } else {
                    body = "404 Not Found\n";
                }

                const std::string response = "HTTP/1.1 200 OK\r\n"
                                             "Content-Type: " +
                                             content_type +
                                             "\r\n"
                                             "Content-Length: " +
                                             std::to_string(body.size()) +
                                             "\r\n"
                                             "Connection: keep-alive\r\n"
                                             "\r\n" +
                                             body;

                self->send(response);
            });
            // Enable 60-second idle timeout
            conn->enable_idle_timeout(true);
            conn->set_idle_timeout(std::chrono::seconds(60));

            return conn;
        });

        while (server.is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        spdlog::info("All connections closed, server exited.");
    } catch (const std::exception &e) {
        spdlog::error("Server error: {}", e.what());
        return 1;
    }

    return 0;
}
