#pragma once

#include "Logger.hpp"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <mcap/writer.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

class McapLogger : public Logger {
public:
    // Opens (or creates) the MCAP file at path.
    explicit McapLogger(const std::string& path);
    ~McapLogger() override;

    void log(const CanFrame& frame,
             const google::protobuf::Descriptor* descriptor,
             const std::string& serialized,
             std::optional<double> update_ratio_ms = std::nullopt) override;

    void log_scalar(const std::string& topic, double value) override;

private:
    mcap::McapWriter writer_;

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
