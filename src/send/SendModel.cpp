#include "SendModel.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
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
    std::vector<double> values;
    for (const SendSignal& ss : messages_[msg_idx].signals)
        values.push_back(ss.value);
    return encode_values(msg_idx, values);
}

std::vector<uint8_t> SendModel::encode_values(std::size_t msg_idx, std::span<const double> values) const {
    const SendMessage& sm = messages_[msg_idx];
    std::vector<uint8_t> data(sm.dlc, 0);
    using EVT = dbcppp::ISignal::EExtendedValueType;
    for (std::size_t i = 0; i < sm.signals.size() && i < values.size(); ++i) {
        const SendSignal& ss = sm.signals[i];
        dbcppp::ISignal::raw_t raw;
        switch (ss.sig->ExtendedValueType()) {
            case EVT::Float: {
                const auto bits = std::bit_cast<uint32_t>(static_cast<float>(values[i]));
                raw = static_cast<dbcppp::ISignal::raw_t>(bits);
                break;
            }
            case EVT::Double: {
                const auto bits = std::bit_cast<uint64_t>(values[i]);
                raw = static_cast<dbcppp::ISignal::raw_t>(bits);
                break;
            }
            default:
                raw = ss.sig->PhysToRaw(values[i]);
                break;
        }
        ss.sig->Encode(raw, data.data());
    }
    return data;
}

int SendModel::find_message(std::string_view name) const {
    for (std::size_t i = 0; i < messages_.size(); ++i)
        if (messages_[i].name == name)
            return static_cast<int>(i);
    return -1;
}

int SendModel::find_signal(std::size_t msg_idx, std::string_view name) const {
    const auto& sigs = messages_[msg_idx].signals;
    for (std::size_t i = 0; i < sigs.size(); ++i)
        if (sigs[i].name == name)
            return static_cast<int>(i);
    return -1;
}
