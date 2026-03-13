#pragma once

#include "Logger.hpp"

#include <mcap/writer.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

class McapLogger : public Logger {
public:
    // Opens (or creates) the MCAP file at path.
    explicit McapLogger(const std::string& path);
    ~McapLogger() override;

    void log(const CanFrame& frame,
             const google::protobuf::Descriptor* descriptor,
             const std::string& serialized) override;

private:
    mcap::McapWriter writer_;

    // topic → channel id  (registered lazily on first message)
    std::unordered_map<std::string, mcap::ChannelId> channels_;

    // proto file name → schema id  (one schema per DBC / interface)
    std::unordered_map<std::string, mcap::SchemaId> schemas_;

    mcap::SchemaId get_or_register_schema(const google::protobuf::Descriptor* descriptor);
    mcap::ChannelId get_or_register_channel(const std::string& topic,
                                             mcap::SchemaId schema_id);
};
