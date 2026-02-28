#include "../include/tcpClient.h"
#include "../include/stringUtils.h"
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

int main(int argc,char* argv[]) {
    // Setup logging
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::info("Client started");

    std::string mode = "text";
    if (argc > 1) mode = argv[1];

    try {
        TcpClient client("127.0.0.1", 9999);
        if (mode == "protobuf") {
            spdlog::info("Running in Protobuf mode");
            while (true) {
                std::cout << "Enter method: ";
                std::string method;
                std::getline(std::cin, method);
                if (method == "quit") break;
                if (method.empty()) continue;

                moderncpp::Request req;
                req.set_method(method);

                if (method == "ECHO") {
                    std::cout << "Enter body: ";
                    std::string body;
                    std::getline(std::cin, body);
                    req.set_body(body);
                }

                client.send_protobuf(req);

                try {
                    moderncpp::Response resp = client.receive_protobuf();
                    spdlog::info("Server response: code={}, message={}, data_size={}",
                                 resp.code(), resp.message(), resp.data().size());
                } catch (const std::exception& e) {
                    spdlog::error("Protobuf receive failed: {}", e.what());
                }
            }
        }else{
            spdlog::info("Running in text mode");
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

                // Retry logic for slow responses
                bool received = false;
                for (int attempt=0;attempt<20;attempt++) {
                    try {
                        if (std::string resp = client.receive_line(); !resp.empty()) {
                            spdlog::info("Server says: {}", trim(rtrim_cc(resp)));
                            received = true;
                            break;
                        }
                    }catch (const net_utils::SyscallException& e) {
                        spdlog::error("Receive attempt {} failed: {}",attempt+1, e.what());
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (!received) {
                    spdlog::warn("No message received after {} attempts",20);
                }
            }
        }
    }catch (const net_utils::SyscallException& e) {
        spdlog::error("Client error: {}", e.what());
    }
    return 0;
}
