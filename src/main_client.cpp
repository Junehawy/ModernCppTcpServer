#include "../include/tcpClient.h"
#include "../include/stringUtils.h"
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

int main() {
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::info("Client started");

    try {
        TcpClient client("127.0.0.1", 9999);
        
        while (true) {
            std::cout << "Enter message (quit to exit): ";
            std::string input;
            std::getline(std::cin, input);

            if (input == "quit") {
                client.send_message("quit");
                break;
            }
            if (input.empty()) continue;
            client.send_message(input);

            bool received = false;
            for (int attempt=0;attempt<20;attempt++) {
                try {
                    std::string resp = client.receive_line();
                    if (!resp.empty()) {
                        spdlog::info("Server says: {}", trim(rtrim_cc(resp)));
                        received = true;
                        break;
                    }
                }catch (const SocketException& e) {
                    spdlog::error("Receive attempt {} failed: {}",attempt+1, e.what());
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!received) {
                spdlog::warn("No message received after {} attempts",20);
            }
        }
    } catch (const SocketException& e) {
        spdlog::error("Client error: {}", e.what());
    }
    return 0;
}
