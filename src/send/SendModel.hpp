#pragma once

#include <dbcppp/Network.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <string>
#include <vector>

struct SendSignal {
    std::string            name;
    std::string            unit;
    double                 min_val;
    double                 max_val;
    double                 value = 0.0;
    const dbcppp::ISignal* sig;   // non-owning, lifetime tied to network_
};

struct SendMessage {
    uint64_t                id;
    std::string             name;
    std::size_t             dlc;
    std::vector<SendSignal> signals; // sorted by start bit
};

class SendModel {
public:
    explicit SendModel(const std::string& dbc_path);

    const std::vector<SendMessage>& messages() const { return messages_; }
    void                            set_value(std::size_t msg_idx, std::size_t sig_idx, double value);
    std::vector<uint8_t>            encode(std::size_t msg_idx) const;

    // Encodes msg_idx with explicit physical values (one per signal, same
    // order as SendMessage::signals). Does not touch the stored values, so it
    // is safe to call concurrently on a model that is no longer modified.
    std::vector<uint8_t>            encode_values(std::size_t msg_idx, std::span<const double> values) const;

    // Lookup by name; -1 if not found.
    int find_message(std::string_view name) const;
    int find_signal(std::size_t msg_idx, std::string_view name) const;

private:
    std::unique_ptr<dbcppp::INetwork> network_;
    std::vector<SendMessage>          messages_; // sorted by ID
};
