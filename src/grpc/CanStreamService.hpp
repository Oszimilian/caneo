#pragma once

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

    grpc::Status Subscribe(grpc::ServerContext* ctx,
                           const caneo::SubscribeRequest* req,
                           grpc::ServerWriter<caneo::RawCanFrame>* writer) override;

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
};
