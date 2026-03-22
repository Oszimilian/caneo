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

class LogController;

class CanStreamService final : public caneo::CanStream::Service {
public:
    // Called from asio thread on every received frame.
    void broadcast(const CanFrame& frame);

    // Signal all active Subscribe() calls to exit (called before server shutdown).
    void shutdown();

    // Optional: called when a client sends a frame via the Send RPC.
    void set_send_fn(SendFn fn) { send_fn_ = std::move(fn); }

    // Attach the server-side log controller so remote clients can start/stop it.
    void set_log_controller(LogController* lc) { log_controller_ = lc; }

    grpc::Status Subscribe(grpc::ServerContext* ctx,
                           const caneo::SubscribeRequest* req,
                           grpc::ServerWriter<caneo::RawCanFrame>* writer) override;

    grpc::Status Send(grpc::ServerContext* ctx,
                      const caneo::RawCanFrame* req,
                      caneo::SendResult* result) override;

    grpc::Status StartLog(grpc::ServerContext* ctx,
                          const caneo::LogControlRequest* req,
                          caneo::LogStatus* status) override;

    grpc::Status StopLog(grpc::ServerContext* ctx,
                         const caneo::LogControlRequest* req,
                         caneo::LogStatus* status) override;

    grpc::Status GetLogStatus(grpc::ServerContext* ctx,
                              const caneo::LogControlRequest* req,
                              caneo::LogStatus* status) override;

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
    LogController*                           log_controller_ = nullptr;
};
