#pragma once
#include "service.grpc.pb.h"

class EchoServiceImpl final : public moderncpp::EchoService::Service {
public:
    grpc::Status Echo(grpc::ServerContext *ctx, const moderncpp::Request *req, moderncpp::Response *resp) override;
    grpc::Status Ping(grpc::ServerContext *ctx, const moderncpp::PingRequest *req,
                      moderncpp::PingResponse *resp) override;
};
