#pragma once
#include <chrono>

// Buffer configuration
constexpr size_t MAX_BUFFER_SIZE = 1 << 20;         // 1MB
constexpr size_t HIGH_WATER_MARK = 640 * 1024;      // 640KB
constexpr size_t LOW_WATER_MARK = 128 * 1024;       // 128KB
constexpr size_t INITIAL_BUFFER_SIZE = 16 * 1024;   // 16KB
constexpr size_t READ_CHUNK_SIZE = 8192;

// Epoll configuration
constexpr int EPOLL_MAX_EVENTS = 1024;
constexpr auto EPOLL_WAIT_TIMEOUT = std::chrono::milliseconds(200);

// Http configuration
constexpr size_t MAX_HTTP_HEADER_SIZE = 64 * 1024;          // 64KB
constexpr size_t MAX_HTTP_BODY_SIZE = 100 * 1024 * 1024;    // 100MB
constexpr size_t MAX_HTTP_PIPELINE = 100;

// Timeout configuration
constexpr auto DEFAULT_IDLE_TIMEOUT = std::chrono::seconds(60);
constexpr auto DEFAULT_CONNECT_TIMEOUT = std::chrono::seconds(5);