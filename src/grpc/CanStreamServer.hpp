#pragma once

#include "CanStreamService.hpp"
#include "action/Action.hpp"

#include <cstdint>
#include <memory>
#include <thread>

namespace grpc { class Server; }

class CanStreamServer {
public:
    explicit CanStreamServer(uint16_t port);
    ~CanStreamServer();

    void broadcast(const CanFrame& frame) { service_.broadcast(frame); }
    void set_send_fn(SendFn fn) { service_.set_send_fn(std::move(fn)); }

private:
    CanStreamService              service_;
    std::unique_ptr<grpc::Server> server_;
    std::thread                   server_thread_;
};
