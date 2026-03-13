#include "SendModel.hpp"

#include <algorithm>
#include <fstream>
#include <ranges>
#include <stdexcept>

SendModel::SendModel(const std::string& dbc_path) {
    std::ifstream f(dbc_path);
    if (!f.is_open())
        throw std::runtime_error("Cannot open DBC: " + dbc_path);
    network_ = dbcppp::INetwork::LoadDBCFromIs(f);
    if (!network_)
        throw std::runtime_error("Cannot parse DBC: " + dbc_path);

    for (const dbcppp::IMessage& msg : network_->Messages()) {
        SendMessage sm;
        sm.id   = msg.Id();
        sm.name = msg.Name();
        sm.dlc  = msg.MessageSize();

        for (const dbcppp::ISignal& sig : msg.Signals()) {
            sm.signals.push_back(SendSignal{
                .name    = std::string(sig.Name()),
                .unit    = std::string(sig.Unit()),
                .min_val = sig.Minimum(),
                .max_val = sig.Maximum(),
                .value   = 0.0,
                .sig     = &sig,
            });
        }
        std::ranges::sort(sm.signals, {}, [](const SendSignal& s) {
            return s.sig->StartBit();
        });
        messages_.push_back(std::move(sm));
    }
    std::ranges::sort(messages_, {}, &SendMessage::id);
}

void SendModel::set_value(std::size_t msg_idx, std::size_t sig_idx, double value) {
    messages_[msg_idx].signals[sig_idx].value = value;
}

std::vector<uint8_t> SendModel::encode(std::size_t msg_idx) const {
    const SendMessage& sm = messages_[msg_idx];
    std::vector<uint8_t> data(sm.dlc, 0);
    for (const SendSignal& ss : sm.signals) {
        const dbcppp::ISignal::raw_t raw = ss.sig->PhysToRaw(ss.value);
        ss.sig->Encode(raw, data.data());
    }
    return data;
}
