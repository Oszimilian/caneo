#pragma once

#include "CanFrame.hpp"

#include <format>
#include <map>
#include <ostream>
#include <sstream>
#include <string>

class DataFrameSet {
public:
    explicit DataFrameSet(std::string interface);

    void update(const CanFrame& frame);

    const std::string& interface() const;
    std::size_t size() const;

    friend std::ostream& operator<<(std::ostream& os, const DataFrameSet& set);

private:
    std::string interface_;
    std::map<uint32_t, CanFrame> frames_;
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
