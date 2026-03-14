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

// ─── Fallback schema (RawCanFrame) ──────────────────────────────────────────

const google::protobuf::Descriptor* McapLogger::get_fallback_descriptor() {
    if (fallback_descriptor_) return fallback_descriptor_;

    google::protobuf::FileDescriptorProto file_proto;
    file_proto.set_name("raw_can_frame.proto");
    file_proto.set_syntax("proto2");

    auto* msg_proto = file_proto.add_message_type();
    msg_proto->set_name("RawCanFrame");

    auto* f = msg_proto->add_field();
    f->set_name("data");
    f->set_number(1);
    f->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT32);
    f->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);

    const auto* file_desc = fallback_pool_.BuildFile(file_proto);
    if (file_desc)
        fallback_descriptor_ = file_desc->FindMessageTypeByName("RawCanFrame");
    return fallback_descriptor_;
}

std::string McapLogger::serialize_fallback(const CanFrame& frame) {
    const auto* desc = get_fallback_descriptor();
    if (!desc) return {};

    std::unique_ptr<google::protobuf::Message> msg(
        fallback_factory_.GetPrototype(desc)->New());
    const auto* refl = msg->GetReflection();

    const auto* data_field = desc->FindFieldByName("data");
    for (const uint8_t byte : frame.payload())
        refl->AddUInt32(msg.get(), data_field, byte);
    std::string out;
    msg->SerializeToString(&out);
    return out;
}

// ─── log() ──────────────────────────────────────────────────────────────────

void McapLogger::log(const CanFrame& frame,
                     const google::protobuf::Descriptor* descriptor,
                     const std::string& serialized)
{
    const std::string topic = descriptor
        ? frame.header().interface + "/" + descriptor->name()
        : frame.header().interface + "/" + std::format("0x{:03X}", frame.header().id);

    const google::protobuf::Descriptor* desc = descriptor
        ? descriptor : get_fallback_descriptor();
    if (!desc) return;

    const std::string fallback_data = descriptor ? std::string{} : serialize_fallback(frame);
    const std::string& data = descriptor ? serialized : fallback_data;

    const mcap::SchemaId  schema_id  = get_or_register_schema(desc);
    const mcap::ChannelId channel_id = get_or_register_channel(topic, schema_id);

    const uint64_t ts = now_ns();

    mcap::Message msg;
    msg.channelId   = channel_id;
    msg.sequence    = 0;
    msg.logTime     = ts;
    msg.publishTime = ts;
    msg.data        = reinterpret_cast<const std::byte*>(data.data());
    msg.dataSize    = data.size();

    const auto status = writer_.write(msg);
    if (!status.ok())
        std::println(stderr, "McapLogger: write error: {}", status.message);
}
