#include "DbcInfo.hpp"

#include <dbcppp/Network.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>

DbcInfo DbcInfo::load(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file.is_open())
        throw std::runtime_error("Failed to open DBC file: " + path.string());
    auto network = dbcppp::INetwork::LoadDBCFromIs(file);
    if (!network)
        throw std::runtime_error("Failed to parse DBC file: " + path.string());

    DbcInfo info;
    info.path = path.string();
    for (const dbcppp::IMessage& m : network->Messages()) {
        Message msg;
        msg.extended = (m.Id() & 0x80000000u) != 0;
        msg.id       = static_cast<uint32_t>(m.Id() & 0x1FFFFFFFu);
        msg.name     = m.Name();
        msg.sender   = m.Transmitter();
        msg.comment  = m.Comment();
        msg.size     = m.MessageSize();
        for (const dbcppp::ISignal& s : m.Signals()) {
            Signal sig;
            sig.name          = s.Name();
            sig.unit          = s.Unit();
            sig.comment       = s.Comment();
            sig.start_bit     = s.StartBit();
            sig.bit_size      = s.BitSize();
            sig.little_endian = s.ByteOrder() == dbcppp::ISignal::EByteOrder::LittleEndian;
            sig.is_signed     = s.ValueType() == dbcppp::ISignal::EValueType::Signed;
            sig.factor        = s.Factor();
            sig.offset        = s.Offset();
            sig.min           = s.Minimum();
            sig.max           = s.Maximum();
            for (uint64_t i = 0; i < s.Receivers_Size(); ++i)
                sig.receivers += (i ? ", " : "") + s.Receivers_Get(i);
            switch (s.MultiplexerIndicator()) {
                case dbcppp::ISignal::EMultiplexer::MuxSwitch: sig.mux = "M"; break;
                case dbcppp::ISignal::EMultiplexer::MuxValue:  sig.mux = "m" + std::to_string(s.MultiplexerSwitchValue()); break;
                default: break;
            }
            for (uint64_t i = 0; i < s.ValueEncodingDescriptions_Size(); ++i) {
                const auto& v = s.ValueEncodingDescriptions_Get(i);
                sig.values.push_back(std::to_string(v.Value()) + " = " + v.Description());
            }
            msg.signals.push_back(std::move(sig));
        }
        std::ranges::sort(msg.signals, {}, &Signal::start_bit);
        info.messages.push_back(std::move(msg));
    }
    std::ranges::sort(info.messages, {}, &Message::id);
    return info;
}

std::ostream& operator<<(std::ostream& os, const DbcInfo& info)
{
    return os << "DbcInfo(" << info.path << ", " << info.messages.size() << " messages)";
}
