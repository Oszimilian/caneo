#pragma once

#include "Socket.hpp"

#include <boost/asio.hpp>
#include <chrono>
#include <optional>
#include <format>
#include <linux/can.h>
#include <ostream>
#include <sstream>
#include <string>

class SocketCAN : public Socket {
public:
    SocketCAN(boost::asio::io_context& io, std::string interface);
    ~SocketCAN() override;

    void start() override;
    void stop() override;

    const std::string& interface() const;
    // Frames with more than 8 bytes (or fd = true) go out as CAN FD frames (with BRS); the length is padded to
    // the next valid CAN FD length.
    void send(uint64_t id, const std::vector<uint8_t>& data, bool fd = false);
    bool fd_enabled() const { return fd_enabled_; }

    friend std::ostream& operator<<(std::ostream& os, const SocketCAN& socket);

private:
    void asyncRead();
    // Reads one frame (with its kernel/hardware RX timestamp) from the socket.
    // Returns true if a frame was read, false on EAGAIN/error (drain finished).
    bool readFrame();

    boost::asio::io_context& io_;
    boost::asio::posix::stream_descriptor descriptor_;
    std::string interface_;
    bool fd_enabled_ = false;   // CAN_RAW_FD_FRAMES accepted
    // Hardware timestamps run on the adapter's / driver's own clock (peak_usb: CLOCK_MONOTONIC, i.e. time
    // since boot), not on the wall clock. hw_offset_ = smallest (wall clock at receive - hw timestamp) seen
    // so far; a hw timestamp + hw_offset_ is a wall clock time with the hardware's precision.
    std::optional<std::chrono::nanoseconds> hw_offset_;
};

template <>
struct std::formatter<SocketCAN> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const SocketCAN& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
