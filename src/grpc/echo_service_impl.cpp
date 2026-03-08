#include "echo_service_impl.h"

#include "spdlog/spdlog.h"

grpc::Status EchoServiceImpl::Echo(grpc::ServerContext *ctx,
                                   const moderncpp::Request *req,
                                   moderncpp::Response *resp) {
    spdlog::info("[gRPC/Echo] method='{}' body_size={} peer={}",
        req->method(),req->body().size(),ctx->peer());

    if (req->method().empty()) {
        return {grpc::StatusCode::INVALID_ARGUMENT,"method must not be empty"};
    }
    resp->set_code(200);
    resp->set_message("OK");
    resp->set_data(req->body());
    return grpc::Status::OK;
}

grpc::Status EchoServiceImpl::Ping(grpc::ServerContext *ctx,
                                   const moderncpp::PingRequest *req,
                                   moderncpp::PingResponse *resp) {
    spdlog::info("[gRPC/Ping] payload='{}' peer={}",
        req->payload(), ctx->peer());

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    resp->set_payload(req->payload());
    resp->set_server_time_ms(now_ms);
    return grpc::Status::OK;
}
