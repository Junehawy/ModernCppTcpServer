#include <iostream>
#include <mutex>
#include <csignal>
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink-inl.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "../include/stringUtils.h"
#include "../include/tcpServer.h"
#include "http_router.h"
#include "http_types.h"
#include "message.pb.h"

HttpRouter g_router;

// Register demo routes
void setup_routes() {
    g_router.get("/", [](const auto &req, auto &resp) {
        resp.body = "Welcome to Tiny HTTP Server!\n";
        resp.content_type = "text/plain";
    });

    // GET /hello
    g_router.get("/hello", [](const auto &req, auto &resp) {
        resp.body = "Hello from tiny HTTP server!\n";
        resp.content_type = "text/plain";
    });

    // GET /json
    g_router.get("/json", [](const auto &req, auto &resp) {
        resp.body = R"({"status":"ok","message":"welcome to my server"})";
        resp.content_type = "application/json";
    });

    // POST /echo
    g_router.post("/echo", [](const auto &req, auto &resp) {
        resp.body = "You sent:\n" + req.body;
        resp.content_type = "text/plain";
    });

    // GET /info
    g_router.get("/info", [](const auto &req, auto &resp) {
        std::string info = "Method: " + req.method + "\n";
        info += "Path: " + req.path + "\n";
        info += "Version: " + req.version + "\n";
        info += "Host: " + req.host + "\n";
        info += "User-Agent: " + req.user_agent + "\n";
        info += "\nHeaders:\n";
        for (const auto &[key, value]: req.headers) {
            info += "  " + key + ": " + value + "\n";
        }

        resp.body = info;
        resp.content_type = "text/plain";
    });
}

// HTTP request handler called from EpollConnection
void handle_http_request(const std::shared_ptr<EpollConnection>& conn,
                        const SimpleHttpRequest& req,
                        const std::string& raw_request) {
    spdlog::info("[HTTP] {} {} from {}", req.method, req.path, conn->get_peer_info());

    const SimpleHttpResponse response = g_router.handle(req);

    conn->send(response.to_string());
}

int main() {
    // Block SIGINT/SIGTERM for clean handling via sigwait
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        std::cerr << "Failed to block signals\n";
        return 1;
    }

    try {
        // Initialize async spdlog with console + rotating file
        spdlog::init_thread_pool(8192, 1);

        const auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::debug);
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");

        const auto file_sink =
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>("logs/server.log", 10 * 1024 * 1024, 5);
        file_sink->set_level(spdlog::level::off);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");

        std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};

        const auto logger = std::make_shared<spdlog::async_logger>(
                "server", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::block);

        spdlog::set_default_logger(logger);
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::info);

        spdlog::info("spdlog initialized");
    } catch (const spdlog::spdlog_ex &ex) {
        std::cerr << "spdlog init failed: " << ex.what() << std::endl;
        return 1;
    }

    setup_routes();
    spdlog::info("Routes registered");

    try {
        TcpServer server(9999, 1024,8);

        // Start server and define client connection handler
        server.start([&](net_utils::SocketPtr client_fd,
                         const sockaddr_in &client_addr) -> std::shared_ptr<BaseConnection> {
            auto conn = std::make_shared<EpollConnection>(std::move(client_fd), client_addr,
                                                          [&](BaseConnection *self, std::string msg) {
                                                              spdlog::info("{} -> {}", self->get_peer_info(), msg);
                                                              const std::string response = rtrim_cc(msg) + "\n";
                                                              self->send(response);
                                                          });

            // Set http hadnler
            conn ->set_http_handler(handle_http_request);

            // Enable 60-second idle timeout
            conn->enable_idle_timeout(true);
            conn->set_idle_timeout(std::chrono::seconds(60));

            return conn;
        });

        // Wait for termination signal
        int sig;
        sigwait(&mask, &sig);
        spdlog::info("Caught signal {}, shutting down ...", sig);

        server.shutdown();
        spdlog::info("Server shutdown complete, exiting.");
    } catch (const std::exception &e) {
        spdlog::error("Server error: {}", e.what());
        return 1;
    }

    return 0;
}
