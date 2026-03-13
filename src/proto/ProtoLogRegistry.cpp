#include "ProtoLogRegistry.hpp"
#include "compat/print.hpp"

void ProtoLogRegistry::add_interface(const std::string& interface, const std::string& dbc_path) {
    if (dbc_path.empty())
        return;
    try {
        logs_[interface] = std::make_unique<ProtoLog>(interface, dbc_path);
    } catch (const std::exception& e) {
        std::println(stderr, "ProtoLogRegistry: {}", e.what());
    }
}

std::string ProtoLogRegistry::serialize(const CanFrame& frame) {
    auto it = logs_.find(frame.header().interface);
    if (it == logs_.end())
        return {};
    return it->second->serialize(frame);
}

std::string ProtoLogRegistry::describe(const CanFrame& frame) {
    auto it = logs_.find(frame.header().interface);
    if (it == logs_.end())
        return {};
    return it->second->describe(frame);
}

const ProtoLog* ProtoLogRegistry::get(const std::string& interface) const {
    auto it = logs_.find(interface);
    return it != logs_.end() ? it->second.get() : nullptr;
}
