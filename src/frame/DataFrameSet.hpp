#pragma once

#include "CanFrame.hpp"

#include <chrono>
#include <format>
#include <map>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>

class DataFrameSet {
public:
    explicit DataFrameSet(std::string interface);

    void update(const CanFrame& frame);

    const std::string& interface() const;
    std::size_t size() const;
    const std::map<uint32_t, CanFrame>& frames() const;
    std::optional<std::chrono::steady_clock::duration> delta(uint32_t id) const;

    friend std::ostream& operator<<(std::ostream& os, const DataFrameSet& set);

private:
    std::string interface_;
    std::map<uint32_t, CanFrame> frames_;
    std::map<uint32_t, std::chrono::steady_clock::duration> deltas_;
};

template <>
struct std::formatter<DataFrameSet> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const DataFrameSet& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
