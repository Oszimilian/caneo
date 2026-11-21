#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

// One CAN channel of a caneo gRPC server (ListInterfaces RPC).
struct RemoteInterface {
    std::string name;
    uint32_t    bitrate      = 0;   // 0 = unknown / virtual
    uint32_t    data_bitrate = 0;   // CAN FD data phase
    bool        fd           = false;
    std::string state;              // "up", "down", "missing", ...

    friend std::ostream& operator<<(std::ostream& os, const RemoteInterface& i);
};

struct RemoteInterfaceList {
    std::string                  host;
    std::vector<RemoteInterface> interfaces;

    friend std::ostream& operator<<(std::ostream& os, const RemoteInterfaceList& l);
};

// Asks the caneo gRPC server at `address` ("host:port") for its channels. Blocking (up to timeout_ms),
// call it from a worker thread. Error text on failure (unreachable, old server without ListInterfaces, ...).
std::expected<RemoteInterfaceList, std::string> list_remote_interfaces(const std::string& address, int timeout_ms = 2000);

// Reset HW on the server (ResetInterfaces RPC): its channels `names` (empty = all) down + up. Blocking.
std::expected<void, std::string> reset_remote_interfaces(const std::string& address, const std::vector<std::string>& names,
                                                         int timeout_ms = 5000);

template <>
struct std::formatter<RemoteInterface> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const RemoteInterface& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};

template <>
struct std::formatter<RemoteInterfaceList> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const RemoteInterfaceList& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
