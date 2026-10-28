#pragma once

#include "ILogControl.hpp"
#include "Logger.hpp"
#include "BlfWriter.hpp"
#include "McapLogger.hpp"
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
    void start() override;

    // Start logging into `path` (directories are created). The extension is
    // replaced per format: .mcap and/or .blf. No-op if already active.
    void start(const std::string& path) override;
    bool supports_path() const override { return true; }

    // Flush and close the current log file. No-op if not active.
    void stop() override;

    bool        is_logging()      const override;
    std::string current_path()    const override;  // empty when not logging; MCAP if both
    std::string current_blf_path() const;          // empty unless writing BLF
    double      elapsed_seconds() const override;  // seconds since start(), 0 if not logging
    std::string last_path()       const override;  // last finished log

    // Print the file paths on start (off in the TUI, stdout belongs to ftxui).
    void set_verbose(bool on) { verbose_ = on; }

    // What to log; applies to the running log immediately.
    void set_options(const LogOptions& options) override;

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
    std::unique_ptr<McapLogger> logger_;   // MCAP (format mcap/both)
    std::unique_ptr<BlfWriter>  blf_;      // BLF  (format blf/both)
    std::string                 current_blf_path_;
    uint64_t                    last_blf_flush_ns_ = 0;

    bool active() const { return logger_ || blf_; }
    std::string             current_path_;
    std::string             last_path_;
    LogOptions              options_;
    bool                    verbose_ = true;
    std::chrono::steady_clock::time_point log_start_time_;

    // Per-(interface, can_id) last-seen timestamp for update_ratio computation.
    std::map<std::pair<std::string, uint32_t>,
             std::chrono::system_clock::time_point> last_ts_;
};
