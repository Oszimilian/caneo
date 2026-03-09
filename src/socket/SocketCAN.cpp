#include "SocketCAN.hpp"

#include "../frame/CanFrame.hpp"

#include <boost/asio/error.hpp>
#include <cerrno>
#include <cstring>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <print>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

SocketCAN::SocketCAN(boost::asio::io_context& io, std::string interface)
    : io_(io), descriptor_(io), interface_(std::move(interface)) {
    int fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        throw std::runtime_error("Failed to open CAN socket: " + std::string(std::strerror(errno)));
    }

    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, interface_.c_str(), IFNAMSIZ - 1);
    if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        ::close(fd);
        throw std::runtime_error("Failed to get interface index for: " + interface_);
    }

    struct sockaddr_can addr {};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error("Failed to bind CAN socket to: " + interface_);
    }

    descriptor_.assign(fd);
}

SocketCAN::~SocketCAN() {
    stop();
}

void SocketCAN::start() {
    asyncRead();
}

void SocketCAN::stop() {
    boost::system::error_code ec;
    descriptor_.close(ec);
}

const std::string& SocketCAN::interface() const {
    return interface_;
}

void SocketCAN::asyncRead() {
    descriptor_.async_read_some(
        boost::asio::buffer(&rawFrame_, sizeof(rawFrame_)),
        [this](boost::system::error_code ec, std::size_t) {
            if (ec) {
                if (ec != boost::asio::error::operation_aborted) {
                    std::println(stderr, "SocketCAN [{}] read error: {}", interface_, ec.message());
                }
                return;
            }

            std::vector<uint8_t> payload(rawFrame_.data, rawFrame_.data + rawFrame_.can_dlc);
            CanHeader header{
                .id = rawFrame_.can_id & CAN_EFF_MASK,
                .interface = interface_,
                .dlc = rawFrame_.can_dlc,
            };

            auto frame = std::make_unique<CanFrame>(std::move(header), std::move(payload));
            if (callback_) {
                callback_(std::move(frame));
            }

            asyncRead();
        });
}

std::ostream& operator<<(std::ostream& os, const SocketCAN& socket) {
    return os << "SocketCAN [" << socket.interface_ << "]";
}
