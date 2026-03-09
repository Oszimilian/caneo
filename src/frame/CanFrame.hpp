#pragma once

#include "DataFrame.hpp"

#include <cstdint>
#include <format>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

struct CanHeader {
    uint32_t id;
    std::string interface;
    uint8_t dlc;
};

class CanFrame : public DataFrame {
public:
    CanFrame(CanHeader header, std::vector<uint8_t> payload);

    const CanHeader& header() const;

    friend std::ostream& operator<<(std::ostream& os, const CanFrame& frame);

private:
    CanHeader header_;
};

template <>
struct std::formatter<CanFrame> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const CanFrame& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
