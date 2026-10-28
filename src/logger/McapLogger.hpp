#pragma once

#include "Logger.hpp"
#include "config/Config.hpp"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <mcap/writer.hpp>

#include <cstdint>
#include <map>
#include <utility>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

class McapLogger : public Logger {
public:
    // Opens (or creates) the MCAP file at path.
    explicit McapLogger(const std::string& path, LogOptions options = {});
    ~McapLogger() override;

    // Takes effect for the next frame.
    void set_options(const LogOptions& options) { options_ = options; }

    // Closes the current chunk and flushes it to disk, so the data written so
    // far survives a crash and can be read (with a fallback scan) while the
    // log is still running. Called automatically every options.flush_s.
    void flush();

    void log(const CanFrame& frame,
             const google::protobuf::Descriptor* descriptor,
             const std::string& serialized,
             std::optional<double> update_ratio_ms = std::nullopt) override;

    void log_scalar(const std::string& topic, double value) override;

private:
    // Log a scalar at an explicit log time (ns since unix epoch).
    void log_scalar_at(const std::string& topic, double value, uint64_t log_time_ns);

    void maybe_flush(uint64_t now);

    mcap::McapWriter writer_;
    LogOptions       options_;
    uint64_t         last_flush_ns_ = 0;

    // (interface, CAN id) -> log time of the last logged frame (min_interval_ms)
    std::map<std::pair<std::string, uint32_t>, uint64_t> last_logged_ns_;

    // topic → channel id  (registered lazily on first message)
    std::unordered_map<std::string, mcap::ChannelId> channels_;

    // proto file name → schema id  (one schema per DBC / interface)
    std::unordered_map<std::string, mcap::SchemaId> schemas_;

    // Fallback schema for frames not found in any DBC
    google::protobuf::DescriptorPool        fallback_pool_;
    google::protobuf::DynamicMessageFactory fallback_factory_;
    const google::protobuf::Descriptor*     fallback_descriptor_ = nullptr;

    // Generic scalar schema — used for update_ratio and model outputs
    google::protobuf::DescriptorPool        scalar_pool_;
    google::protobuf::DynamicMessageFactory scalar_factory_;
    const google::protobuf::Descriptor*     scalar_descriptor_ = nullptr;

    // Raw frame schema — used for playback (can_id + raw bytes)
    google::protobuf::DescriptorPool        raw_pool_;
    google::protobuf::DynamicMessageFactory raw_factory_;
    const google::protobuf::Descriptor*     raw_descriptor_ = nullptr;

    const google::protobuf::Descriptor* get_fallback_descriptor();
    std::string serialize_fallback(const CanFrame& frame);

    const google::protobuf::Descriptor* get_scalar_descriptor();
    std::string serialize_scalar(double value);

    const google::protobuf::Descriptor* get_raw_descriptor();
    std::string serialize_raw(const CanFrame& frame);

    mcap::SchemaId get_or_register_schema(const google::protobuf::Descriptor* descriptor);
    mcap::ChannelId get_or_register_channel(const std::string& topic,
                                             mcap::SchemaId schema_id);
};
