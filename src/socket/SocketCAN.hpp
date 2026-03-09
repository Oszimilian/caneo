#pragma once

#include "Socket.hpp"

#include <boost/asio.hpp>
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

    friend std::ostream& operator<<(std::ostream& os, const SocketCAN& socket);

private:
    void asyncRead();

    boost::asio::io_context& io_;
    boost::asio::posix::stream_descriptor descriptor_;
    std::string interface_;
    struct can_frame rawFrame_ {};
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
