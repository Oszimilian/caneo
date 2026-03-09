#include "DataFrameSet.hpp"

#include <bitset>
#include <iomanip>

DataFrameSet::DataFrameSet(std::string interface)
    : interface_(std::move(interface)) {}

void DataFrameSet::update(const CanFrame& frame) {
    frames_.insert_or_assign(frame.header().id, frame);
}

const std::string& DataFrameSet::interface() const {
    return interface_;
}

std::size_t DataFrameSet::size() const {
    return frames_.size();
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
        os << "\n";
    }
    return os;
}
