#pragma once

#include "frame/CanFrame.hpp"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

// One ProtoLog per interface.
// At construction, reads the DBC and builds a protobuf Descriptor for every
// CAN message (one proto message type with all signals as double fields).
// serialize() encodes a decoded CanFrame into protobuf binary format.
class ProtoLog {
public:
    ProtoLog(std::string interface, const std::string& dbc_path);

    const std::string& interface() const { return interface_; }

    // Returns the Descriptor for a given CAN message ID, or nullptr if unknown.
    const google::protobuf::Descriptor* descriptor(uint64_t msg_id) const;

    // Serializes the decoded signals of a CanFrame into protobuf binary.
    // Returns an empty string if no descriptor exists for the frame's message ID.
    std::string serialize(const CanFrame& frame);

    // Returns a human-readable "field: value" representation of the frame.
    // Returns an empty string if no descriptor exists for the frame's message ID.
    std::string describe(const CanFrame& frame);

private:
    std::string interface_;

    // Pool + factory must be declared in this order (factory references pool)
    google::protobuf::DescriptorPool        pool_;
    google::protobuf::DynamicMessageFactory factory_;

    // CAN message ID → protobuf Descriptor
    std::unordered_map<uint64_t, const google::protobuf::Descriptor*> descriptors_;

    // CAN message ID → (original signal name → sanitized proto field name)
    std::unordered_map<uint64_t, std::unordered_map<std::string, std::string>> field_names_;

    std::unique_ptr<google::protobuf::Message> build_message(const CanFrame& frame);
};
