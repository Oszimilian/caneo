#include "DecoderRegistry.hpp"

#include "DbcPppWrapper.hpp"

void DecoderRegistry::add_interface(const std::string& interface, const std::filesystem::path& dbc_path) {
    decoders_[interface] = std::make_unique<DbcPppWrapper>(dbc_path);
}

void DecoderRegistry::decode(CanFrame& frame) {
    const auto& iface = frame.header().interface;
    auto it = decoders_.find(iface);
    if (it == decoders_.end()) {
        throw std::runtime_error("No decoder registered for interface: " + iface);
    }
    it->second->decode(frame);
}
