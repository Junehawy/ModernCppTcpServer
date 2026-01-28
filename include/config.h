#pragma once
#include <chrono>
constexpr size_t MAX_BUFFER_SIZE = 1 << 20; // 1MB
constexpr int EPOLL_MAX_EVENTS = 1024;
constexpr size_t READ_CHUNK_SIZE = 8192;
constexpr std::chrono::seconds DEFAULT_IDLE_TIMEOUT = std::chrono::seconds(60);
constexpr std::chrono::milliseconds EPOLL_WAIT_TIMEOUT = std::chrono::milliseconds(200);