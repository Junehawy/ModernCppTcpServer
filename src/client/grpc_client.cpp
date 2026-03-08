#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include "service.grpc.pb.h"
#include "spdlog/spdlog.h"

static void demo_echo(moderncpp::EchoService::Stub &stub, const std::string &body) {
    moderncpp::Request req;
    req.set_method("ECHO");
    req.set_body(body);

    moderncpp::Response resp;
    grpc::ClientContext ctx;

    const grpc::Status status = stub.Echo(&ctx, req, &resp);
    if (status.ok()) {
        spdlog::info("Echo response: code={} msg='{}' data='{}'",
                     resp.code(), resp.message(),
                     std::string(resp.data().begin(), resp.data().end()));
    } else {
        spdlog::error("Echo RPC failed: {} - {}",
                      static_cast<int>(status.error_code()), status.error_message());
    }
}

static void demo_ping(moderncpp::EchoService::Stub &stub, const std::string &payload) {
    moderncpp::PingRequest req;
    req.set_payload(payload.empty()
        ? "ping_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count())
        : payload);

    moderncpp::PingResponse resp;
    grpc::ClientContext ctx;

    const grpc::Status status = stub.Ping(&ctx, req, &resp);
    if (status.ok()) {
        spdlog::info("Ping response: payload='{}' server_time={}ms",
                     resp.payload(), resp.server_time_ms());
    } else {
        spdlog::error("Ping RPC failed: {}", status.error_message());
    }
}

static void print_help() {
    fmt::print(
        "\nCommands:\n"
        "  echo <message>   Send echo request\n"
        "  ping [payload]   Send ping request\n"
        "  help             Show this message\n"
        "  quit             Exit\n\n"
    );
}

int main(int argc, char *argv[]) {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    const std::string target = (argc > 1) ? argv[1] : "localhost:50051";
    spdlog::info("Connecting to {}", target);

    const auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    const auto stub    = moderncpp::EchoService::NewStub(channel);

    print_help();

    std::string line;
    while (true) {
        fmt::print("grpc> ");
        if (!std::getline(std::cin, line)) break;

        const auto sep  = line.find(' ');
        const std::string cmd = line.substr(0, sep);
        const std::string arg = (sep != std::string::npos) ? line.substr(sep + 1) : "";

        if (cmd == "quit" || cmd == "q") {
            break;
        } else if (cmd == "echo") {
            if (arg.empty()) {
                fmt::print("usage: echo <message>\n");
                continue;
            }
            demo_echo(*stub, arg);
        } else if (cmd == "ping") {
            demo_ping(*stub, arg);
        } else if (cmd == "help" || cmd == "h") {
            print_help();
        } else if (cmd.empty()) {
            continue;
        } else {
            fmt::print("Unknown command '{}'. Type 'help' for usage.\n", cmd);
        }
    }

    spdlog::info("Bye.");
    return 0;
}