#pragma once

#include <dbcppp/Network.h>

#include <cstdint>
#include <memory>
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

private:
    std::unique_ptr<dbcppp::INetwork> network_;
    std::vector<SendMessage>          messages_; // sorted by ID
};
