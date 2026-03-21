#include "CanStreamServer.hpp"

#include "compat/print.hpp"

#include <grpcpp/grpcpp.h>

CanStreamServer::CanStreamServer(uint16_t port) {
    const std::string addr = "0.0.0.0:" + std::to_string(port);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    std::println("gRPC server listening on {}", addr);
    server_thread_ = std::thread([this] { server_->Wait(); });
}

CanStreamServer::~CanStreamServer() {
    service_.shutdown();
    server_->Shutdown();
    if (server_thread_.joinable())
        server_thread_.join();
}
