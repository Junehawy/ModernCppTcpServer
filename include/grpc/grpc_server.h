#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <grpcpp/grpcpp.h>

class GrpcServer {
public:
    explicit GrpcServer(std::string address);
    ~GrpcServer();

    GrpcServer(const GrpcServer &) = delete;
    GrpcServer &operator=(const GrpcServer &) = delete;

    void start();
    void shutdown();
    bool is_running() const { return running_.load(); }

private:
    std::string address_;
    std::unique_ptr<grpc::Server> server_;
    std::atomic<bool> running_{false};
};
