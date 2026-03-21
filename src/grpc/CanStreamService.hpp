#pragma once

#include "action/Action.hpp"
#include "frame/CanFrame.hpp"
#include "can_stream.grpc.pb.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

class CanStreamService final : public caneo::CanStream::Service {
public:
    // Called from asio thread on every received frame.
    void broadcast(const CanFrame& frame);

    // Signal all active Subscribe() calls to exit (called before server shutdown).
    void shutdown();

    // Optional: called when a client sends a frame via the Send RPC.
    void set_send_fn(SendFn fn) { send_fn_ = std::move(fn); }

    grpc::Status Subscribe(grpc::ServerContext* ctx,
                           const caneo::SubscribeRequest* req,
                           grpc::ServerWriter<caneo::RawCanFrame>* writer) override;

    grpc::Status Send(grpc::ServerContext* ctx,
                      const caneo::RawCanFrame* req,
                      caneo::SendResult* result) override;

private:
    struct Subscriber {
        std::string                    interface_filter;
        std::queue<caneo::RawCanFrame> queue;
        std::mutex                     mutex;
        std::condition_variable        cv;
        std::atomic<bool>              done{false};
    };

    std::vector<std::shared_ptr<Subscriber>> subscribers_;
    std::mutex                               subs_mutex_;
    SendFn                                   send_fn_;
};
