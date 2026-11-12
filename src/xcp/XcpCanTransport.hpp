#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

// XCP on CAN / CAN FD (ASAM XCP part 3): one XCP packet per CAN frame, no transport header. Uses its own raw
// socket that only receives the slave's ID, so it does not depend on the Live sockets. Blocking, used by the
// XcpMaster worker thread only. IDs with bit 31 set (or > 0x7FF) are 29-bit IDs.
class XcpCanTransport {
public:
    XcpCanTransport(std::string interface, uint32_t tx_id, uint32_t rx_id, bool fd, bool pad);   // throws
    ~XcpCanTransport();
    XcpCanTransport(const XcpCanTransport&) = delete;
    XcpCanTransport& operator=(const XcpCanTransport&) = delete;

    // Throws std::runtime_error on a socket error (bus off, interface down, ...).
    void send(std::span<const uint8_t> packet);
    // Next packet of the slave, nullopt after `timeout`. Throws on a socket error.
    std::optional<std::vector<uint8_t>> receive(std::chrono::microseconds timeout);

    std::size_t max_packet() const { return fd_ ? 64 : 8; }
    bool        fd() const { return fd_; }
    bool        pad() const { return pad_; }
    void        set_pad(bool pad) { pad_ = pad; }
    const std::string& interface() const { return interface_; }

    friend std::ostream& operator<<(std::ostream& os, const XcpCanTransport& t);

private:
    std::string interface_;
    uint32_t    tx_id_, rx_id_;
    bool        fd_, pad_;
    int         sock_ = -1;
};

template <>
struct std::formatter<XcpCanTransport> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const XcpCanTransport& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
