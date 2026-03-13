#pragma once

#include "ProtoLog.hpp"

#include <memory>
#include <string>
#include <unordered_map>

class ProtoLogRegistry {
public:
    // Creates a ProtoLog for the given interface using the DBC file at dbc_path.
    // No-op if dbc_path is empty.
    void add_interface(const std::string& interface, const std::string& dbc_path);

    // Serializes the decoded signals of a CanFrame into protobuf binary.
    // Returns an empty string if no ProtoLog exists for the frame's interface
    // or if the message ID has no descriptor.
    std::string serialize(const CanFrame& frame);
    std::string describe(const CanFrame& frame);

    // Direct access to a ProtoLog by interface name, or nullptr.
    const ProtoLog* get(const std::string& interface) const;

private:
    std::unordered_map<std::string, std::unique_ptr<ProtoLog>> logs_;
};
