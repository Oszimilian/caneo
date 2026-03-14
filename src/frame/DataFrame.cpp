#include "DataFrame.hpp"

#include <bitset>

DataFrame::DataFrame(std::vector<uint8_t> payload)
    : payload_(std::move(payload))
    , timestamp_(std::chrono::steady_clock::now()) {}

const std::vector<uint8_t>& DataFrame::payload() const {
    return payload_;
}

const std::vector<DecodedSignal>& DataFrame::decoded() const {
    return decoded_;
}

std::chrono::steady_clock::time_point DataFrame::timestamp() const {
    return timestamp_;
}

void DataFrame::addDecoded(DecodedSignal signal) {
    decoded_.push_back(std::move(signal));
}

const std::string& DataFrame::msg_name() const {
    return msg_name_;
}

void DataFrame::set_msg_name(std::string name) {
    msg_name_ = std::move(name);
}

std::ostream& operator<<(std::ostream& os, const DataFrame& frame) {
    os << "DataFrame:\n";
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
