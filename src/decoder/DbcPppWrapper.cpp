#include "DbcPppWrapper.hpp"

#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>

DbcPppWrapper::DbcPppWrapper(const std::filesystem::path& dbc_path) {
    std::ifstream file(dbc_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open DBC file: " + dbc_path.string());
    }
    network_ = dbcppp::INetwork::LoadDBCFromIs(file);
    if (!network_) {
        throw std::runtime_error("Failed to parse DBC file: " + dbc_path.string());
    }
    for (const dbcppp::IMessage& msg : network_->Messages()) {
        messages_.emplace(msg.Id(), &msg);
    }
}

void DbcPppWrapper::decode(CanFrame& frame) {
    const void* raw = frame.payload().data();

    auto it = messages_.find(frame.header().id);
    if (it == messages_.end()) return;

    const dbcppp::IMessage* msg = it->second;
    if (frame.payload().size() < msg->MessageSize())
        return;
    frame.set_msg_name(std::string(msg->Name()));
    const dbcppp::ISignal* mux_sig = msg->MuxSignal();

    for (const dbcppp::ISignal& sig : msg->Signals()) {
        if (sig.MultiplexerIndicator() == dbcppp::ISignal::EMultiplexer::MuxValue &&
            (!mux_sig || mux_sig->Decode(raw) != sig.MultiplexerSwitchValue())) {
            continue;
        }
        double y_min = sig.Minimum();
        double y_max = sig.Maximum();
        if (y_min == 0.0 && y_max == 0.0)
            y_max = std::pow(2.0, static_cast<double>(sig.BitSize()));

        double phys = sig.RawToPhys(sig.Decode(raw));

        // Some DBC files declare a signal as signed ('-') but define a non-negative
        // physical range (e.g. [0|16383]).  When raw values have the sign-bit set
        // dbcppp correctly sign-extends them, producing negative physical values that
        // contradict the declared range.  Detect this and re-interpret the raw bits
        // as unsigned so the value lands in [dbc_min, dbc_max].
        if (phys < y_min && y_min >= 0.0 && y_max > y_min) {
            const auto     raw_bits = static_cast<uint64_t>(sig.Decode(raw));
            const uint64_t mask     = (sig.BitSize() < 64)
                                      ? ((uint64_t{1} << sig.BitSize()) - 1u)
                                      : ~uint64_t{0};
            phys = static_cast<double>(raw_bits & mask)
                   * sig.Factor() + sig.Offset();
        }

        frame.addDecoded(DecodedSignal{
            .name    = std::string(sig.Name()),
            .value   = phys,
            .unit    = std::string(sig.Unit()),
            .min_val = y_min,
            .max_val = y_max,
        });
    }
}
