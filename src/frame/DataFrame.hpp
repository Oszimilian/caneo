#pragma once

#include <cstdint>
#include <format>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

struct DecodedSignal {
    std::string name;
    double value;
    std::string unit;
};

class DataFrame {
public:
    explicit DataFrame(std::vector<uint8_t> payload);
    virtual ~DataFrame() = default;

    const std::vector<uint8_t>& payload() const;
    const std::vector<DecodedSignal>& decoded() const;
    void addDecoded(DecodedSignal signal);

    friend std::ostream& operator<<(std::ostream& os, const DataFrame& frame);

protected:
    std::vector<uint8_t> payload_;
    std::vector<DecodedSignal> decoded_;
};

template <>
struct std::formatter<DataFrame> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const DataFrame& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
