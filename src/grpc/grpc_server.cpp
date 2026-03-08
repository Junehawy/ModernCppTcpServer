#include "grpc/grpc_server.h"
#include "common/config.h"
#include "echo_service_impl.h"
#include "spdlog/spdlog.h"

GrpcServer::GrpcServer(std::string address):address_(std::move(address)){
}
GrpcServer::~GrpcServer() { shutdown(); }
void GrpcServer::start() {
    static EchoServiceImpl echo_svc;

    grpc::ServerBuilder builder;
    builder.AddListeningPort(address_,grpc::InsecureServerCredentials());

    builder.RegisterService(&echo_svc);

    builder.SetMaxSendMessageSize(GRPC_MAX_MESSAGE_SIZE);
    builder.SetMaxReceiveMessageSize(GRPC_MAX_MESSAGE_SIZE);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS,GRPC_KEEPALIVE_TIME_MS);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,GRPC_KEEPALIVE_TIMEOUT_MS);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS,1);

    server_ = builder.BuildAndStart();
    if (!server_) throw std::runtime_error("[gRPC:(] Failed to start server");
    running_ = true;
    spdlog::info("[gRPC] Server Listening on {}",address_);

}

void GrpcServer::shutdown() {
    if (!running_.exchange(false)) return;
    if (server_) {
        const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(GRPC_SHUTDOWN_TIMEOUT_S);
        server_->Shutdown(deadline);
        spdlog::info("[gRPC] Shutdown completed successfully");
    }
}