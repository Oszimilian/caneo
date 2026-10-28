#include "LogController.hpp"
#include "McapLogger.hpp"

#include "compat/print.hpp"

#include <filesystem>

LogController::LogController(const AppConfig& app, ProtoLogRegistry& proto_registry)
    : app_(app), proto_registry_(proto_registry), options_(app.config.logging) {}

void LogController::set_options(const LogOptions& options) {
    std::lock_guard lock(mutex_);
    options_ = options;
    if (logger_) logger_->set_options(options);
}

void LogController::start() {
    start(app_.make_log_path());
}

void LogController::start(const std::string& path) {
    std::lock_guard lock(mutex_);
    if (active()) return;
    std::filesystem::path p = expand_path(path);
    if (p.extension() == ".mcap" || p.extension() == ".blf")
        p.replace_extension();
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path());

    // Open both before committing, so a failure leaves nothing half-started.
    std::unique_ptr<McapLogger> mcap;
    std::unique_ptr<BlfWriter>  blf;
    const std::string mcap_path = p.string() + ".mcap";
    const std::string blf_path  = p.string() + ".blf";
    if (options_.mcap()) mcap = std::make_unique<McapLogger>(mcap_path, options_);
    if (options_.blf())  blf  = std::make_unique<BlfWriter>(blf_path);

    logger_            = std::move(mcap);
    blf_               = std::move(blf);
    current_path_      = logger_ ? mcap_path : blf_path;
    current_blf_path_  = blf_ ? blf_path : std::string{};
    last_blf_flush_ns_ = 0;
    log_start_time_    = std::chrono::steady_clock::now();
    if (verbose_) {
        if (logger_) std::println("McapLogger: writing to {}", mcap_path);
        if (blf_)    std::println("BlfWriter: writing to {}", blf_path);
    }
}

std::string LogController::current_blf_path() const {
    std::lock_guard lock(mutex_);
    return current_blf_path_;
}

std::string LogController::last_path() const {
    std::lock_guard lock(mutex_);
    return last_path_;
}

void LogController::stop() {
    std::lock_guard lock(mutex_);
    if (active())
        last_path_ = current_path_;
    logger_.reset();
    blf_.reset();
    current_blf_path_.clear();
    current_path_.clear();
    last_ts_.clear();
    log_start_time_ = {};
}

bool LogController::is_logging() const {
    std::lock_guard lock(mutex_);
    return active();
}

std::string LogController::current_path() const {
    std::lock_guard lock(mutex_);
    return current_path_;
}

double LogController::elapsed_seconds() const {
    std::lock_guard lock(mutex_);
    if (!active()) return 0.0;
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - log_start_time_).count();
}

void LogController::log_can_frame(const CanFrame& frame) {
    std::lock_guard lock(mutex_);
    if (!active()) return;

    // BLF: every frame, raw (thinning / raw / update_ratio only apply to MCAP)
    if (blf_) {
        blf_->write(frame);
        if (options_.flush_s > 0) {
            const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            if (last_blf_flush_ns_ == 0) last_blf_flush_ns_ = now;
            if (now - last_blf_flush_ns_ >= static_cast<uint64_t>(options_.flush_s) * 1'000'000'000ULL) {
                blf_->flush();
                last_blf_flush_ns_ = now;
            }
        }
    }
    if (!logger_) return;

    // Period since the previous frame of this ID - computed on every frame,
    // even when the logger drops some (min_interval_ms).
    const auto key = std::make_pair(frame.header().interface, frame.header().id);
    std::optional<double> ratio_ms;
    if (const auto it = last_ts_.find(key); it != last_ts_.end())
        ratio_ms = std::chrono::duration<double, std::milli>(
            frame.timestamp() - it->second).count();
    last_ts_[key] = frame.timestamp();

    const std::string serialized = proto_registry_.serialize(frame);
    const auto* proto_log = proto_registry_.get(frame.header().interface);
    const auto* desc      = proto_log ? proto_log->descriptor(frame.header().id) : nullptr;

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
