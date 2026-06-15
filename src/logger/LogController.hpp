#pragma once

#include "ILogControl.hpp"
#include "Logger.hpp"
#include "config/AppConfig.hpp"
#include "frame/CanFrame.hpp"
#include "proto/ProtoLogRegistry.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

// Thread-safe logger wrapper that allows starting and stopping an MCAP log at
// runtime. Implements Logger so it can be passed directly to ModelEngine.
//
// log_can_frame() is called from the asio thread; start()/stop() are called
// from the GUI (render) thread. A mutex protects the inner logger.
class LogController : public ILogControl, public Logger {
public:
    LogController(const AppConfig& app, ProtoLogRegistry& proto_registry);

    // Create a new timestamped MCAP file and start logging. No-op if already active.
    void start();

    // Flush and close the current log file. No-op if not active.
    void stop();

    bool        is_logging()      const;
    std::string current_path()    const;  // empty when not logging
    double      elapsed_seconds() const;  // seconds since start(), 0 if not logging

    // Full log-frame pipeline (decode proto, compute update_ratio, write).
    // Thread-safe; called from the asio thread.
    void log_can_frame(const CanFrame& frame);

    // Logger interface — forwarded to the inner McapLogger under lock.
    void log(const CanFrame& frame,
             const google::protobuf::Descriptor* descriptor,
             const std::string& serialized,
             std::optional<double> update_ratio_ms = std::nullopt) override;

    void log_scalar(const std::string& topic, double value) override;

private:
    const AppConfig&  app_;
    ProtoLogRegistry& proto_registry_;

    mutable std::mutex      mutex_;
    std::unique_ptr<Logger> logger_;
    std::string             current_path_;
    std::chrono::steady_clock::time_point log_start_time_;

    // Per-(interface, can_id) last-seen timestamp for update_ratio computation.
    std::map<std::pair<std::string, uint32_t>,
             std::chrono::system_clock::time_point> last_ts_;
};
