#include "RemoteLogControl.hpp"

#include <grpcpp/grpcpp.h>

RemoteLogControl::RemoteLogControl(const std::string& server_address) {
    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    stub_        = caneo::CanStream::NewStub(channel);
    refresh_status();
}

void RemoteLogControl::start() {
    grpc::ClientContext ctx;
    caneo::LogControlRequest req;
    caneo::LogStatus status;
    stub_->StartLog(&ctx, req, &status);
    logging_ = status.active();
    path_    = status.path();
    if (logging_) log_start_time_ = std::chrono::steady_clock::now();
}

void RemoteLogControl::stop() {
    grpc::ClientContext ctx;
    caneo::LogControlRequest req;
    caneo::LogStatus status;
    stub_->StopLog(&ctx, req, &status);
    logging_ = status.active();
    path_    = status.path();
}

void RemoteLogControl::refresh_status() {
    grpc::ClientContext ctx;
    caneo::LogControlRequest req;
    caneo::LogStatus status;
    const auto result = stub_->GetLogStatus(&ctx, req, &status);
    if (result.ok()) {
        const bool was_logging = logging_;
        logging_ = status.active();
        path_    = status.path();
        // If the server was already logging when we connected, start the local timer now.
        if (logging_ && !was_logging)
            log_start_time_ = std::chrono::steady_clock::now();
    }
}

double RemoteLogControl::elapsed_seconds() const {
    if (!logging_) return 0.0;
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - log_start_time_).count();
}
