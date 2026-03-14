#pragma once
#include <chrono>

// Buffer configuration
constexpr size_t MAX_BUFFER_SIZE = 4 << 20;             // 4MB
constexpr size_t HIGH_WATER_MARK = 2* 1024 * 1024;      // 2MB
constexpr size_t LOW_WATER_MARK = 256 * 1024;           // 128KB
constexpr size_t INITIAL_BUFFER_SIZE = 128 * 1024;      // 16KB
constexpr size_t READ_CHUNK_SIZE = 65536;

// Epoll configuration
constexpr int EPOLL_MAX_EVENTS = 2048;
constexpr auto EPOLL_WAIT_TIMEOUT = std::chrono::milliseconds(100);

// Http configuration
constexpr size_t MAX_HTTP_HEADER_SIZE = 128 * 1024;             // 128KB
constexpr size_t MAX_HTTP_BODY_SIZE = 256 * 1024 * 1024;        // 256MB
constexpr size_t MAX_HTTP_PIPELINE = 100;

// Timeout configuration
constexpr auto DEFAULT_IDLE_TIMEOUT = std::chrono::seconds(60);
constexpr auto DEFAULT_CONNECT_TIMEOUT = std::chrono::seconds(5);

// TcpServer configuration
constexpr size_t MAX_ACCEPT_PER_LOOP = 100;

// gRRPC configuration
constexpr int GRPC_MAX_MESSAGE_SIZE   = 4 * 1024 * 1024;  // 4 MB
constexpr int GRPC_KEEPALIVE_TIME_MS  = 30000;
constexpr int GRPC_KEEPALIVE_TIMEOUT_MS = 5000;
constexpr int GRPC_SHUTDOWN_TIMEOUT_S = 5;