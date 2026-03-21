#pragma once

#include "Socket.hpp"
#include "can_stream.grpc.pb.h"

#include <boost/asio.hpp>
#include <atomic>
#include <format>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>

class SocketGrpc : public Socket {
public:
    SocketGrpc(boost::asio::io_context& io, std::string interface, std::string server_address);
    ~SocketGrpc() override;

    void start() override;
    void stop() override;

    const std::string& interface() const { return interface_; }

    friend std::ostream& operator<<(std::ostream& os, const SocketGrpc& s);

private:
    boost::asio::io_context&                io_;
    std::string                             interface_;
    std::string                             server_address_;
    std::atomic<bool>                       running_{false};
    std::thread                             reader_thread_;
    std::unique_ptr<grpc::ClientContext>    ctx_;
    std::unique_ptr<caneo::CanStream::Stub> stub_;
};

template <>
struct std::formatter<SocketGrpc> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const SocketGrpc& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
