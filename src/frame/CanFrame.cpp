#include "CanFrame.hpp"

#include <bitset>
#include <iomanip>

CanFrame::CanFrame(CanHeader header, std::vector<uint8_t> payload)
    : DataFrame(std::move(payload)), header_(std::move(header)) {}

const CanHeader& CanFrame::header() const {
    return header_;
}

std::ostream& operator<<(std::ostream& os, const CanFrame& frame) {
    os << "CanFrame [" << frame.header_.interface << "]"
       << " ID: 0x" << std::hex << std::uppercase
       << std::setw(3) << std::setfill('0') << frame.header_.id
       << std::dec << " DLC: " << static_cast<int>(frame.header_.dlc) << "\n";
    os << "  Payload [" << frame.payload_.size() << "]:";
    for (auto byte : frame.payload_) {
        os << " " << std::bitset<8>(byte);
    }
    if (!frame.decoded_.empty()) {
        os << "\n  Decoded:";
        for (const auto& sig : frame.decoded_) {
            os << "\n    " << sig.name << ": " << sig.value << " " << sig.unit;
        }
    }
    return os;
}
