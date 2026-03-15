cmake_minimum_required(VERSION 3.15)
project(ModernCSProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(Dependancies)
include(ProtoGen)

add_library(tcp_socket STATIC
        src/net/base_connection.cpp
        src/net/epoll_connection.cpp
        src/net/sub_reactor.cpp
        src/net/tcp_server.cpp
        src/http/http_types.cpp
        src/common/worker_pool.cpp
        src/common/server_metrics.cpp
)
target_include_directories(tcp_socket PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(tcp_socket PUBLIC
        spdlog::spdlog
        pthread
        proto_lib
)

add_library(grpc_lib STATIC
        src/grpc/grpc_server.cpp
        src/grpc/echo_service_impl.cpp
)
target_include_directories(grpc_lib PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(grpc_lib PUBLIC
        spdlog::spdlog
        proto_lib
)

add_executable(server
        src/server/main.cpp
        src/server/routes.cpp
)

target_link_libraries(server PRIVATE
        tcp_socket
        grpc_lib
)

add_executable(client
        src/client/main.cpp
        src/client/tcp_client.cpp
)

target_include_directories(grpc_lib PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(client PRIVATE
        pthread
        spdlog::spdlog
        proto_lib
)

add_executable(grpc_client
        src/client/grpc_client.cpp
)
target_link_libraries(grpc_client PRIVATE
        proto_lib
        spdlog::spdlog
)