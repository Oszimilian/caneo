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
        logging_ = status.active();
        path_    = status.path();
    }
}
