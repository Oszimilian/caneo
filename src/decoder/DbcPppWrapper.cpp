#include "DbcPppWrapper.hpp"

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
    const dbcppp::ISignal* mux_sig = msg->MuxSignal();

    for (const dbcppp::ISignal& sig : msg->Signals()) {
        if (sig.MultiplexerIndicator() == dbcppp::ISignal::EMultiplexer::MuxValue &&
            (!mux_sig || mux_sig->Decode(raw) != sig.MultiplexerSwitchValue())) {
            continue;
        }
        frame.addDecoded(DecodedSignal{
            .name  = std::string(sig.Name()),
            .value = sig.RawToPhys(sig.Decode(raw)),
            .unit  = std::string(sig.Unit()),
        });
    }
}
