// NOTE: Do NOT define MCAP_IMPLEMENTATION here — it is already defined in McapLogger.cpp.
#include "McapReader.hpp"
#include "compat/print.hpp"

#include <mcap/reader.hpp>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include <chrono>
#include <fstream>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>

// ─── Helpers ─────────────────────────────────────────────────────────────────

namespace {

struct SchemaState {
    google::protobuf::DescriptorPool        pool;
    google::protobuf::DynamicMessageFactory factory;
    const google::protobuf::Descriptor*     desc = nullptr;
};

enum class ChannelKind { Data, Raw, Other };

struct ChannelInfo {
    ChannelKind kind;
    std::string iface;
    std::string msg_name;
};

// Parse "vcan0/MsgName/data" or "vcan0/MsgName/raw".
// Returns Other if neither suffix matches.
ChannelInfo classify_topic(const std::string& topic)
{
    for (const auto& [suffix, kind] : {
            std::pair{std::string("/data"), ChannelKind::Data},
            std::pair{std::string("/raw"),  ChannelKind::Raw}})
    {
        if (topic.size() <= suffix.size()) continue;
        if (topic.compare(topic.size() - suffix.size(), suffix.size(), suffix) != 0) continue;

        const std::string base = topic.substr(0, topic.size() - suffix.size());
        const auto sep = base.find('/');
        if (sep == std::string::npos) continue;

        return {kind, base.substr(0, sep), base.substr(sep + 1)};
    }
    return {ChannelKind::Other, {}, {}};
}

// For DBC message names, derive a stable CAN ID from the name hash.
// For hex-format names like "0x1A3", parse directly.
uint32_t derive_can_id(const std::string& msg_name)
{
    if (msg_name.size() > 2 &&
        msg_name[0] == '0' && (msg_name[1] == 'x' || msg_name[1] == 'X'))
    {
        try {
            return static_cast<uint32_t>(
                std::stoul(msg_name.substr(2), nullptr, 16));
        } catch (...) {}
    }
    return static_cast<uint32_t>(std::hash<std::string>{}(msg_name) & 0x7FFu);
}

} // namespace

// ─── McapReader ──────────────────────────────────────────────────────────────

McapReader::McapReader(const std::string& path) : path_(path) {}

std::vector<std::string> McapReader::scan_interfaces() const
{
    std::ifstream file(path_, std::ios::binary);
    if (!file.is_open()) return {};

    mcap::FileStreamReader stream(file);
    mcap::McapReader reader;
    if (!reader.open(stream).ok()) return {};

    reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);

    std::set<std::string> ifaces;
    for (const auto& [id, channel] : reader.channels()) {
        const auto info = classify_topic(channel->topic);
        if (info.kind == ChannelKind::Raw && !info.iface.empty())
            ifaces.insert(info.iface);
    }
    reader.close();
    return {ifaces.begin(), ifaces.end()};
}

void McapReader::play(const OnFrame& on_frame, const OnSend& on_send)
{
    std::ifstream file(path_, std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("McapReader: cannot open file: " + path_);

    mcap::FileStreamReader stream(file);
    mcap::McapReader reader;
    {
        const auto status = reader.open(stream);
        if (!status.ok())
            throw std::runtime_error("McapReader: " + std::string(status.message));
    }

    reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);

    // Build a protobuf SchemaState per schema for deserialization.
    std::unordered_map<mcap::SchemaId, std::unique_ptr<SchemaState>> schemas;
    for (const auto& [id, schema] : reader.schemas()) {
        if (schema->encoding != "protobuf") continue;

        auto state = std::make_unique<SchemaState>();

        google::protobuf::FileDescriptorSet fds;
        if (!fds.ParseFromArray(
                reinterpret_cast<const char*>(schema->data.data()),
                static_cast<int>(schema->data.size())))
            continue;

        for (const auto& file_proto : fds.file())
            state->pool.BuildFile(file_proto);

        state->desc = state->pool.FindMessageTypeByName(std::string(schema->name));
        if (state->desc)
            schemas[id] = std::move(state);
    }

    // Classify all channels.
    std::unordered_map<mcap::ChannelId, ChannelInfo> channel_map;
    for (const auto& [id, channel] : reader.channels()) {
        auto info = classify_topic(channel->topic);
        if (info.kind != ChannelKind::Other)
            channel_map[id] = std::move(info);
    }

    // Iterate all messages in chronological order.
    bool     started    = false;
    uint64_t t0_ns      = 0;
    std::chrono::steady_clock::time_point start_time;

    for (const auto& view : reader.readMessages()) {
        const auto ch_it = channel_map.find(view.channel->id);
        if (ch_it == channel_map.end()) continue;

        const auto& info = ch_it->second;

        // Pace to original timing.
        if (!started) {
            started    = true;
            t0_ns      = view.message.logTime;
            start_time = std::chrono::steady_clock::now();
        }
        const auto target = start_time +
            std::chrono::nanoseconds(view.message.logTime - t0_ns);
        std::this_thread::sleep_until(target);

        const auto schema_it = schemas.find(view.channel->schemaId);
        if (schema_it == schemas.end()) continue;
        const auto* desc = schema_it->second->desc;
        if (!desc) continue;

        std::unique_ptr<google::protobuf::Message> proto_msg(
            schema_it->second->factory.GetPrototype(desc)->New());
        if (!proto_msg->ParseFromArray(
                reinterpret_cast<const char*>(view.message.data),
                static_cast<int>(view.message.dataSize)))
            continue;

        const auto* refl = proto_msg->GetReflection();

        if (info.kind == ChannelKind::Data && on_frame) {
            // Reconstruct CanFrame with decoded signals for the TUI.
            CanHeader header;
            header.id        = derive_can_id(info.msg_name);
            header.interface = info.iface;
            header.dlc       = 0;
            auto frame = std::make_unique<CanFrame>(header, std::vector<uint8_t>{});
            frame->set_msg_name(info.msg_name);

            for (int i = 0; i < desc->field_count(); ++i) {
                const auto* field = desc->field(i);
                if (field->type() != google::protobuf::FieldDescriptor::TYPE_DOUBLE)
                    continue;
                DecodedSignal sig;
                sig.name  = field->name();
                sig.value = refl->GetDouble(*proto_msg, field);
                sig.unit  = "";
                frame->addDecoded(sig);
            }

            on_frame(std::move(frame));

        } else if (info.kind == ChannelKind::Raw && on_send) {
            // Extract real CAN ID and raw payload bytes (repeated uint32) for socket send.
            const auto* id_field = desc->FindFieldByName("can_id");
            const auto* pl_field = desc->FindFieldByName("payload");
            if (!id_field || !pl_field) continue;

            const uint32_t can_id = refl->GetUInt32(*proto_msg, id_field);
            const int pl_size = refl->FieldSize(*proto_msg, pl_field);
            std::vector<uint8_t> payload;
            payload.reserve(pl_size);
            for (int i = 0; i < pl_size; ++i)
                payload.push_back(static_cast<uint8_t>(
                    refl->GetRepeatedUInt32(*proto_msg, pl_field, i)));

            on_send(info.iface, can_id, payload);
        }
    }

    reader.close();
    std::println("McapReader: playback finished.");
}
