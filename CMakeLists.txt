cmake_minimum_required(VERSION 3.15)
project(ModernCSProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include_directories(include)

add_library(tcp_socket STATIC
    src/lib_src/tcpServer.cpp
    src/lib_src/tcpClient.cpp
    src/lib_src/socketUtils.cpp
    src/lib_src/Connection.cpp
)

target_link_directories(tcp_socket PRIVATE pthread)

add_executable(server src/main_server.cpp)
add_executable(client src/main_client.cpp)

target_link_libraries(server PRIVATE tcp_socket pthread)
target_link_libraries(client PRIVATE tcp_socket pthread)

add_subdirectory(extern/spdlog)

target_link_libraries(tcp_socket PUBLIC spdlog::spdlog)
target_link_libraries(server PRIVATE tcp_socket spdlog::spdlog pthread)
target_link_libraries(client PRIVATE tcp_socket spdlog::spdlog pthread)