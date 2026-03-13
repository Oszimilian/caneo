#include "ProtoLog.hpp"
#include "compat/print.hpp"

#include <dbcppp/Network.h>
#include <google/protobuf/message.h>

#include <cctype>
#include <fstream>
#include <stdexcept>

// ─── Helpers ───────────────────────────────────────────────────────────────

static std::string sanitize(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out[0])))
        out = "_" + out;
    return out.empty() ? "_" : out;
}

// ─── Construction ──────────────────────────────────────────────────────────

ProtoLog::ProtoLog(std::string interface, const std::string& dbc_path)
    : interface_(std::move(interface))
    , factory_(&pool_)
{
    std::ifstream f(dbc_path);
    if (!f.is_open())
        throw std::runtime_error("ProtoLog: cannot open DBC: " + dbc_path);

    auto network = dbcppp::INetwork::LoadDBCFromIs(f);
    if (!network)
        throw std::runtime_error("ProtoLog: cannot parse DBC: " + dbc_path);

    // Build one FileDescriptorProto containing all CAN messages as proto types
    google::protobuf::FileDescriptorProto file_proto;
    file_proto.set_name(interface_ + ".proto");
    file_proto.set_syntax("proto3");

    for (const dbcppp::IMessage& msg : network->Messages()) {
        if (msg.Name() == "VECTOR__INDEPENDENT_SIG_MSG")
            continue;
        auto* msg_proto = file_proto.add_message_type();
        msg_proto->set_name(sanitize(std::string(msg.Name())));

        auto& sig_map = field_names_[msg.Id()];
        int   field_num = 1;

        for (const dbcppp::ISignal& sig : msg.Signals()) {
            const std::string original = std::string(sig.Name());
            const std::string safe     = sanitize(original);
            sig_map[original]          = safe;

            auto* field = msg_proto->add_field();
            field->set_name(safe);
            field->set_number(field_num++);
            field->set_type(google::protobuf::FieldDescriptorProto::TYPE_DOUBLE);
            field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
        }
    }

    const google::protobuf::FileDescriptor* file_desc = pool_.BuildFile(file_proto);
    if (!file_desc) {
        throw std::runtime_error(
            "ProtoLog: failed to build protobuf descriptors for interface: " + interface_);
    }

    // Map each CAN message ID to its Descriptor
    for (const dbcppp::IMessage& msg : network->Messages()) {
        if (msg.Name() == "VECTOR__INDEPENDENT_SIG_MSG")
            continue;
        const std::string msg_name = sanitize(std::string(msg.Name()));
        const google::protobuf::Descriptor* desc = file_desc->FindMessageTypeByName(msg_name);
        if (desc)
            descriptors_[msg.Id()] = desc;
        else
            std::println(stderr, "ProtoLog [{}]: no descriptor for '{}'", interface_, msg_name);
    }
}

// ─── Accessors ─────────────────────────────────────────────────────────────

const google::protobuf::Descriptor* ProtoLog::descriptor(uint64_t msg_id) const {
    auto it = descriptors_.find(msg_id);
    return it != descriptors_.end() ? it->second : nullptr;
}

// ─── Serialization ─────────────────────────────────────────────────────────

std::unique_ptr<google::protobuf::Message> ProtoLog::build_message(const CanFrame& frame) {
    const uint64_t id = frame.header().id;

    auto desc_it = descriptors_.find(id);
    if (desc_it == descriptors_.end())
        return nullptr;

    auto names_it = field_names_.find(id);
    if (names_it == field_names_.end())
        return nullptr;

    const google::protobuf::Descriptor*  desc      = desc_it->second;
    const auto&                          name_map  = names_it->second;
    const google::protobuf::Message*     prototype = factory_.GetPrototype(desc);
    std::unique_ptr<google::protobuf::Message> msg(prototype->New());
    const google::protobuf::Reflection*  refl      = msg->GetReflection();

    for (const DecodedSignal& sig : frame.decoded()) {
        auto name_it = name_map.find(sig.name);
        if (name_it == name_map.end()) continue;

        const google::protobuf::FieldDescriptor* field =
            desc->FindFieldByName(name_it->second);
        if (field)
            refl->SetDouble(msg.get(), field, sig.value);
    }

    return msg;
}

std::string ProtoLog::serialize(const CanFrame& frame) {
    auto msg = build_message(frame);
    if (!msg) return {};
    std::string out;
    msg->SerializeToString(&out);
    return out;
}

std::string ProtoLog::describe(const CanFrame& frame) {
    auto msg = build_message(frame);
    if (!msg) return {};

    const google::protobuf::Descriptor*  desc = msg->GetDescriptor();
    const google::protobuf::Reflection*  refl = msg->GetReflection();

    std::string out;
    for (int i = 0; i < desc->field_count(); ++i) {
        const google::protobuf::FieldDescriptor* field = desc->field(i);
        if (!refl->HasField(*msg, field)) continue;
        out += std::format("  {}: {}\n", field->name(), refl->GetDouble(*msg, field));
    }
    return out;
}
