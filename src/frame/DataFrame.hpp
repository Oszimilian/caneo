#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

struct DecodedSignal {
    std::string name;
    double      value;
    std::string unit;
    double      min_val = 0.0;
    double      max_val = 0.0;
};

class DataFrame {
public:
    explicit DataFrame(std::vector<uint8_t> payload);
    virtual ~DataFrame() = default;

    const std::vector<uint8_t>& payload() const;
    const std::vector<DecodedSignal>& decoded() const;
    void addDecoded(DecodedSignal signal);

    const std::string& msg_name() const;
    void set_msg_name(std::string name);

    // Receive time of the frame. Defaults to the construction time (wall clock),
    // but SocketCAN overrides it with the kernel/hardware RX timestamp when
    // available (see set_timestamp / SO_TIMESTAMPING). system_clock domain so it
    // maps directly to a unix-epoch log time.
    std::chrono::system_clock::time_point timestamp() const;
    void set_timestamp(std::chrono::system_clock::time_point ts);

    friend std::ostream& operator<<(std::ostream& os, const DataFrame& frame);

protected:
    std::vector<uint8_t> payload_;
    std::vector<DecodedSignal> decoded_;
    std::string msg_name_;
    std::chrono::system_clock::time_point timestamp_;
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
