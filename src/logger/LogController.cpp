#include "LogController.hpp"
#include "McapLogger.hpp"

LogController::LogController(const AppConfig& app, ProtoLogRegistry& proto_registry)
    : app_(app), proto_registry_(proto_registry) {}

void LogController::start() {
    std::lock_guard lock(mutex_);
    if (logger_) return;
    current_path_ = app_.make_log_path();
    logger_        = std::make_unique<McapLogger>(current_path_);
}

void LogController::stop() {
    std::lock_guard lock(mutex_);
    logger_.reset();
    current_path_.clear();
    last_ts_.clear();
}

bool LogController::is_logging() const {
    std::lock_guard lock(mutex_);
    return logger_ != nullptr;
}

std::string LogController::current_path() const {
    std::lock_guard lock(mutex_);
    return current_path_;
}

void LogController::log_can_frame(const CanFrame& frame) {
    std::lock_guard lock(mutex_);
    if (!logger_) return;

    const std::string serialized = proto_registry_.serialize(frame);
    const auto* proto_log = proto_registry_.get(frame.header().interface);
    const auto* desc      = proto_log ? proto_log->descriptor(frame.header().id) : nullptr;

    const auto key = std::make_pair(frame.header().interface, frame.header().id);
    std::optional<double> ratio_ms;
    if (const auto it = last_ts_.find(key); it != last_ts_.end())
        ratio_ms = std::chrono::duration<double, std::milli>(
            frame.timestamp() - it->second).count();
    last_ts_[key] = frame.timestamp();

    logger_->log(frame, desc, serialized, ratio_ms);
}

void LogController::log(const CanFrame& frame,
                        const google::protobuf::Descriptor* descriptor,
                        const std::string& serialized,
                        std::optional<double> update_ratio_ms) {
    std::lock_guard lock(mutex_);
    if (logger_) logger_->log(frame, descriptor, serialized, update_ratio_ms);
}

void LogController::log_scalar(const std::string& topic, double value) {
    std::lock_guard lock(mutex_);
    if (logger_) logger_->log_scalar(topic, value);
}
