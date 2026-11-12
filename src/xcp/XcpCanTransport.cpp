#include "XcpCanTransport.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr uint32_t XCP_EXT_FLAG = 0x80000000u;   // XCP convention for 29-bit IDs

canid_t to_can_id(uint32_t id)
{
    const bool ext = (id & XCP_EXT_FLAG) || (id & ~XCP_EXT_FLAG) > CAN_SFF_MASK;
    id &= ~XCP_EXT_FLAG;
    return ext ? ((id & CAN_EFF_MASK) | CAN_EFF_FLAG) : (id & CAN_SFF_MASK);
}

std::size_t fd_len(std::size_t n)
{
    constexpr std::size_t LENS[] = {12, 16, 20, 24, 32, 48, 64};
    if (n <= 8) return n;
    for (auto l : LENS)
        if (n <= l) return l;
    return 64;
}

std::runtime_error sys_error(const std::string& what)
{
    return std::runtime_error(what + ": " + std::strerror(errno));
}

}   // namespace

XcpCanTransport::XcpCanTransport(std::string interface, uint32_t tx_id, uint32_t rx_id, bool fd, bool pad)
    : interface_(std::move(interface)), tx_id_(tx_id), rx_id_(rx_id), fd_(fd), pad_(pad)
{
    sock_ = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (sock_ < 0) throw sys_error("XCP: cannot open CAN socket");

    const auto fail = [&](const std::string& what) {
        auto e = sys_error(what);
        ::close(sock_);
        sock_ = -1;
        return e;
    };

    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, interface_.c_str(), IFNAMSIZ - 1);
    if (::ioctl(sock_, SIOCGIFINDEX, &ifr) < 0) throw fail("XCP: interface " + interface_ + " not found");
    const int ifindex = ifr.ifr_ifindex;   // ifr is reused for the MTU below (union)

    if (fd_) {
        int on = 1;
        if (::setsockopt(sock_, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof(on)) < 0)
            throw fail("XCP: CAN FD not supported");
        // CAN FD frames can only be sent on an interface with MTU 72 (fd on / vcan mtu 72)
        if (::ioctl(sock_, SIOCGIFMTU, &ifr) == 0 && ifr.ifr_mtu != CANFD_MTU) {
            ::close(sock_);
            sock_ = -1;
            throw std::runtime_error("XCP: " + interface_ + " is not in CAN FD mode (MTU " + std::to_string(ifr.ifr_mtu) +
                                     ") - enable CAN FD in its Baudrate block and press Start");
        }
    }

    const canid_t rx = to_can_id(rx_id_);
    struct can_filter filter {
        .can_id   = rx,
        .can_mask = (rx & CAN_EFF_FLAG ? CAN_EFF_MASK : CAN_SFF_MASK) | CAN_EFF_FLAG | CAN_RTR_FLAG,
    };
    if (::setsockopt(sock_, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)) < 0)
        throw fail("XCP: cannot set the CAN filter");

    struct sockaddr_can addr {};
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifindex;
    if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw fail("XCP: cannot bind to " + interface_);
}

XcpCanTransport::~XcpCanTransport()
{
    if (sock_ >= 0) ::close(sock_);
}

void XcpCanTransport::send(std::span<const uint8_t> packet)
{
    if (packet.size() > max_packet())
        throw std::runtime_error(std::format("XCP: packet of {} bytes does not fit into a CAN{} frame", packet.size(),
                                             fd_ ? " FD" : ""));
    const canid_t id = to_can_id(tx_id_);
    ssize_t n = 0;
    std::size_t want = 0;
    if (fd_) {
        struct canfd_frame f {};
        f.can_id = id;
        std::size_t len = pad_ ? std::max<std::size_t>(packet.size(), 8) : packet.size();
        f.len   = static_cast<uint8_t>(fd_len(len));
        f.flags = CANFD_BRS;
        std::ranges::copy(packet, f.data);
        want = CANFD_MTU;
        n    = ::write(sock_, &f, CANFD_MTU);
    } else {
        struct can_frame f {};
        f.can_id = id;
        f.len    = static_cast<uint8_t>(pad_ ? 8 : packet.size());
        std::ranges::copy(packet, f.data);
        want = CAN_MTU;
        n    = ::write(sock_, &f, CAN_MTU);
    }
    if (n < 0) {
        if (errno == ENOBUFS) {   // TX queue full: give the bus a moment, then once more
            ::usleep(2000);
            return send(packet);
        }
        throw sys_error("XCP: send on " + interface_ + " failed");
    }
    if (static_cast<std::size_t>(n) != want) throw std::runtime_error("XCP: short write on " + interface_);
}

std::optional<std::vector<uint8_t>> XcpCanTransport::receive(std::chrono::microseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        struct pollfd p { .fd = sock_, .events = POLLIN, .revents = 0 };
        const int r = ::poll(&p, 1, static_cast<int>(std::max<int64_t>(left.count(), 0)));
        if (r < 0) {
            if (errno == EINTR) continue;
            throw sys_error("XCP: poll failed");
        }
        if (r == 0) return std::nullopt;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
            throw std::runtime_error("XCP: socket error on " + interface_ + " (interface down?)");

        struct canfd_frame f {};
        const ssize_t n = ::read(sock_, &f, sizeof(f));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            throw sys_error("XCP: receive on " + interface_ + " failed");
        }
        if (static_cast<std::size_t>(n) != CAN_MTU && static_cast<std::size_t>(n) != CANFD_MTU) continue;
        if (f.can_id & (CAN_ERR_FLAG | CAN_RTR_FLAG)) continue;
        const std::size_t len = std::min<std::size_t>(f.len, static_cast<std::size_t>(n) == CANFD_MTU ? 64 : 8);
        if (len == 0) continue;
        return std::vector<uint8_t>(f.data, f.data + len);
    }
}

std::ostream& operator<<(std::ostream& os, const XcpCanTransport& t)
{
    return os << "XcpCanTransport(" << t.interface_ << (t.fd_ ? ", CAN FD" : ", CAN") << ", tx 0x" << std::hex
              << t.tx_id_ << ", rx 0x" << t.rx_id_ << std::dec << ")";
}
