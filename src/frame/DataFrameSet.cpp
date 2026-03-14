#include "DataFrameSet.hpp"

#include <bitset>
#include <iomanip>

DataFrameSet::DataFrameSet(std::string interface)
    : interface_(std::move(interface)) {}

void DataFrameSet::update(const CanFrame& frame) {
    const uint32_t id = frame.header().id;
    if (const auto it = frames_.find(id); it != frames_.end()) {
        deltas_[id] = frame.timestamp() - it->second.timestamp();
        if (frame.decoded().empty() && !it->second.decoded().empty()) {
            // New frame not decoded (e.g. DLC mismatch) — keep previous signals
            CanFrame merged = frame;
            for (const auto& sig : it->second.decoded())
                merged.addDecoded(sig);
            if (merged.msg_name().empty())
                merged.set_msg_name(it->second.msg_name());
            frames_.insert_or_assign(id, std::move(merged));
            return;
        }
    }
    frames_.insert_or_assign(id, frame);
}

const std::string& DataFrameSet::interface() const {
    return interface_;
}

std::size_t DataFrameSet::size() const {
    return frames_.size();
}

const std::map<uint32_t, CanFrame>& DataFrameSet::frames() const {
    return frames_;
}

std::optional<std::chrono::steady_clock::duration> DataFrameSet::delta(uint32_t id) const {
    if (const auto it = deltas_.find(id); it != deltas_.end())
        return it->second;
    return std::nullopt;
}

std::ostream& operator<<(std::ostream& os, const DataFrameSet& set) {
    os << "DataFrameSet [" << set.interface_ << "] (" << set.frames_.size() << " frames):\n";
    for (const auto& [id, frame] : set.frames_) {
        os << "  0x" << std::hex << std::uppercase
           << std::setw(3) << std::setfill('0') << id
           << std::dec << " | DLC:" << static_cast<int>(frame.header().dlc) << " |";
        for (auto byte : frame.payload()) {
            os << " " << std::bitset<8>(byte);
        }
        for (const auto& sig : frame.decoded()) {
            os << "\n    " << sig.name << ": " << sig.value << " " << sig.unit;
        }
        os << "\n";
    }
    return os;
}
