#include "SignalStore.hpp"

void SignalStore::update(const CanFrame& frame) {
    const std::string prefix =
        frame.header().interface + "/" + frame.msg_name() + "/";
    for (const auto& sig : frame.decoded())
        values_[prefix + sig.name] = sig.value;
}

std::optional<double> SignalStore::get(const std::string& iface,
                                       const std::string& msg,
                                       const std::string& signal) const {
    const std::string key = iface + "/" + msg + "/" + signal;
    if (const auto it = values_.find(key); it != values_.end())
        return it->second;
    return std::nullopt;
}
