#pragma once

#include "frame/CanFrame.hpp"

#include <optional>
#include <string>
#include <unordered_map>

// Flat key-value store: "<iface>/<msg_name>/<signal_name>" → current value.
// Written and read exclusively from the asio thread.
class SignalStore {
public:
    // Update all decoded signals from a received frame.
    void update(const CanFrame& frame);

    // Retrieve a signal value; nullopt if not yet seen.
    std::optional<double> get(const std::string& iface,
                              const std::string& msg,
                              const std::string& signal) const;

private:
    std::unordered_map<std::string, double> values_;
};
