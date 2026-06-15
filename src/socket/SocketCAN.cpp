#include "SocketCAN.hpp"

#include "../frame/CanFrame.hpp"

#include <boost/asio/error.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/net_tstamp.h>
#include <net/if.h>
#include <optional>
#include "compat/print.hpp"
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

// SCM_TIMESTAMPING equals SO_TIMESTAMPING; define a fallback in case the libc
// headers don't expose the SCM alias.
#ifndef SCM_TIMESTAMPING
#define SCM_TIMESTAMPING SO_TIMESTAMPING
#endif

namespace {
// Layout of the SCM_TIMESTAMPING control message: ts[0] = software,
// ts[2] = raw hardware. Defined locally to avoid pulling in <linux/errqueue.h>.
struct ScmTimestamping {
    struct timespec ts[3];
};
}  // namespace

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

    // Echo own transmitted frames back so they appear in the trace view
    int recv_own = 1;
    if (::setsockopt(fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &recv_own, sizeof(recv_own)) < 0) {
        ::close(fd);
        throw std::runtime_error("Failed to set CAN_RAW_RECV_OWN_MSGS on: " + interface_);
    }

    // Ask the kernel to attach RX timestamps to incoming frames. Hardware
    // timestamps are used when the controller provides them, otherwise the
    // kernel software (driver RX) timestamp is delivered. Best-effort: if the
    // kernel/driver doesn't support it we silently fall back to userspace time.
    int ts_flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE |
                   SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
    if (::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &ts_flags, sizeof(ts_flags)) < 0) {
        std::println(stderr,
                     "SocketCAN [{}]: SO_TIMESTAMPING unavailable ({}), using userspace time",
                     interface_, std::strerror(errno));
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
    // Wait for readiness, then read with recvmsg() so we can pick up the
    // SCM_TIMESTAMPING ancillary data (async_read_some can't carry control msgs).
    descriptor_.async_wait(
        boost::asio::posix::stream_descriptor::wait_read,
        [this](boost::system::error_code ec) {
            if (ec) {
                if (ec != boost::asio::error::operation_aborted) {
                    std::println(stderr, "SocketCAN [{}] read error: {}", interface_, ec.message());
                }
                return;
            }

            // asio uses edge-triggered epoll, so drain everything currently
            // queued before re-arming the wait.
            while (readFrame()) {}

            asyncRead();
        });
}

bool SocketCAN::readFrame() {
    struct can_frame raw {};
    struct iovec iov {
        .iov_base = &raw,
        .iov_len  = sizeof(raw),
    };
    alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(ScmTimestamping))]{};
    struct msghdr msg {};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = control;
    msg.msg_controllen = sizeof(control);

    const ssize_t n = ::recvmsg(descriptor_.native_handle(), &msg, 0);
    if (n < 0)
        return false;  // EAGAIN/EWOULDBLOCK (drained) or error
    if (static_cast<std::size_t>(n) < sizeof(raw))
        return true;   // runt frame, skip but keep draining

    // Extract the RX timestamp (prefer hardware, fall back to software).
    std::optional<std::chrono::system_clock::time_point> ts;
    for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm != nullptr; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_TIMESTAMPING)
            continue;
        ScmTimestamping scm {};
        std::memcpy(&scm, CMSG_DATA(cm), sizeof(scm));
        const timespec& hw = scm.ts[2];
        const timespec& sw = scm.ts[0];
        const timespec* chosen = (hw.tv_sec || hw.tv_nsec) ? &hw
                               : (sw.tv_sec || sw.tv_nsec) ? &sw
                                                           : nullptr;
        if (chosen)
            ts = std::chrono::system_clock::time_point(
                std::chrono::duration_cast<std::chrono::system_clock::duration>(
                    std::chrono::seconds(chosen->tv_sec) +
                    std::chrono::nanoseconds(chosen->tv_nsec)));
        break;
    }

    std::vector<uint8_t> payload(raw.data, raw.data + raw.can_dlc);
    CanHeader header{
        .id        = raw.can_id & CAN_EFF_MASK,
        .interface = interface_,
        .dlc       = raw.can_dlc,
    };

    auto frame = std::make_unique<CanFrame>(std::move(header), std::move(payload));
    if (ts)
        frame->set_timestamp(*ts);  // else keep the construction-time fallback
    if (callback_)
        callback_(std::move(frame));

    return true;
}

void SocketCAN::send(uint64_t id, const std::vector<uint8_t>& data) {
    struct can_frame frame{};
    frame.can_id  = static_cast<canid_t>(id & CAN_EFF_MASK);
    frame.can_dlc = static_cast<uint8_t>(std::min(data.size(), std::size_t{8}));
    std::copy_n(data.begin(), frame.can_dlc, frame.data);
    boost::asio::write(descriptor_, boost::asio::buffer(&frame, sizeof(frame)));
}

std::ostream& operator<<(std::ostream& os, const SocketCAN& socket) {
    return os << "SocketCAN [" << socket.interface_ << "]";
}
