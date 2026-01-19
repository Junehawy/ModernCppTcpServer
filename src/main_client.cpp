#include "../include/tcpClient.h"
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

int main() {
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::info("Client started");

    try {
        TcpClient client("127.0.0.1", 9999);
        char buf[1024];
        
        while (true) {
            std::cout << "Enter message (quit to exit): ";
            std::cout.flush();
            if (!fgets(buf, sizeof(buf), stdin))
                break;
            buf[strcspn(buf, "\n")] = '\0';
            std::string cmd = trim(buf);
            if (to_lower(cmd) == "quit") {
                client.send_message("quit");
                break;
            }
            client.send_message(buf);
            std::string resp = client.receive_line();
            if (resp.empty())
                break;
            spdlog::debug("Server: {}", trim(rtrim_cc(resp)));
        }
    } catch (const std::exception& e) {
        spdlog::error("Client error: {}", e.what());
    }
    return 0;
}