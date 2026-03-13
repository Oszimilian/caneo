#define MCAP_IMPLEMENTATION
#include "McapLogger.hpp"
#include "compat/print.hpp"

#include <google/protobuf/descriptor.pb.h>

#include <chrono>
#include <stdexcept>

// ─── Helpers ────────────────────────────────────────────────────────────────

static uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
        .count());
}

// ─── Construction ───────────────────────────────────────────────────────────

McapLogger::McapLogger(const std::string& path) {
    mcap::McapWriterOptions opts("");  // profile = ""
    const auto status = writer_.open(path, opts);
    if (!status.ok())
        throw std::runtime_error("McapLogger: cannot open file: " + path +
                                 " (" + std::string(status.message) + ")");
    std::println("McapLogger: writing to {}", path);
}

McapLogger::~McapLogger() {
    writer_.close();
}

// ─── Schema / channel registration ─────────────────────────────────────────

mcap::SchemaId McapLogger::get_or_register_schema(
    const google::protobuf::Descriptor* descriptor)
{
    // Key and schema name = fully-qualified message type (e.g. "SS_ELMO_TARGET").
    // PlotJuggler / Foxglove look up the descriptor by this name.
    const std::string key = descriptor->full_name();
    auto it = schemas_.find(key);
    if (it != schemas_.end())
        return it->second;

    // Serialize the FileDescriptorSet so the reader can reconstruct the type.
    google::protobuf::FileDescriptorSet fds;
    descriptor->file()->CopyTo(fds.add_file());
    std::string schema_data;
    fds.SerializeToString(&schema_data);

    mcap::Schema schema;
    schema.name     = descriptor->full_name();
    schema.encoding = "protobuf";
    schema.data     = mcap::ByteArray(
        reinterpret_cast<const std::byte*>(schema_data.data()),
        reinterpret_cast<const std::byte*>(schema_data.data() + schema_data.size()));
    writer_.addSchema(schema);

    schemas_[key] = schema.id;
    return schema.id;
}

mcap::ChannelId McapLogger::get_or_register_channel(const std::string& topic,
                                                      mcap::SchemaId schema_id)
{
    auto it = channels_.find(topic);
    if (it != channels_.end())
        return it->second;

    mcap::Channel channel;
    channel.topic           = topic;
    channel.messageEncoding = "protobuf";
    channel.schemaId        = schema_id;
    writer_.addChannel(channel);

    channels_[topic] = channel.id;
    return channel.id;
}

// ─── log() ──────────────────────────────────────────────────────────────────

void McapLogger::log(const CanFrame& frame,
                     const google::protobuf::Descriptor* descriptor,
                     const std::string& serialized)
{
    if (!descriptor)
        return;

    const std::string topic =
        frame.header().interface + "/" + descriptor->name();

    const mcap::SchemaId  schema_id  = get_or_register_schema(descriptor);
    const mcap::ChannelId channel_id = get_or_register_channel(topic, schema_id);

    const uint64_t ts = now_ns();

    mcap::Message msg;
    msg.channelId   = channel_id;
    msg.sequence    = 0;
    msg.logTime     = ts;
    msg.publishTime = ts;
    msg.data        = reinterpret_cast<const std::byte*>(serialized.data());
    msg.dataSize    = serialized.size();

    const auto status = writer_.write(msg);
    if (!status.ok())
        std::println(stderr, "McapLogger: write error: {}", status.message);
}
