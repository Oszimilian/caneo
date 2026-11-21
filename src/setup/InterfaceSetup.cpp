#include "InterfaceSetup.hpp"
#include "compat/print.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <cerrno>
#include <cstring>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <stdexcept>
#include <unistd.h>

static bool interface_exists(const std::string& name) {
    return if_nametoindex(name.c_str()) != 0;
}

static bool interface_is_up(const std::string& name) {
    std::ifstream f("/sys/class/net/" + name + "/operstate");
    std::string state;
    if (f >> state)
        return state == "up" || state == "unknown";
    return false;
}

// CAN FD interfaces (and vcan devices that carry FD frames) have an MTU of 72 (CANFD_MTU), classic ones 16.
static bool interface_is_fd(const std::string& name) {
    std::ifstream f("/sys/class/net/" + name + "/mtu");
    int mtu = 0;
    return (f >> mtu) && mtu == 72;
}

static bool safe_name(const std::string& n) {
    return !n.empty() && n.size() < IFNAMSIZ &&
           std::ranges::all_of(n, [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.'; });
}

std::vector<std::string> plan_setup(const Config& cfg, std::vector<std::string>* warnings) {
    std::vector<std::string> cmds;
    const auto is_virtual = [&](const InterfaceConfig& i) { return cfg.virtual_can || i.virtual_can; };
    const auto warn = [&](std::string w) { if (warnings) warnings->push_back(std::move(w)); };

    bool need_vcan_module = false;
    std::vector<std::string> per_iface;
    for (const auto& iface : cfg.interfaces) {
        if (iface.is_remote()) continue;   // gRPC channel: the server sets its bus up
        if (!safe_name(iface.name)) {
            warn(std::format("invalid interface name '{}'", iface.name));
            continue;
        }
        if (is_virtual(iface)) {
            need_vcan_module = true;
            const bool exists = interface_exists(iface.name);
            if (!exists)
                per_iface.push_back(std::format("ip link add dev {} type vcan", iface.name));
            const bool mtu_wrong = iface.fd && (!exists || !interface_is_fd(iface.name));
            if (mtu_wrong) {
                if (exists && interface_is_up(iface.name))
                    per_iface.push_back(std::format("ip link set down {}", iface.name));
                per_iface.push_back(std::format("ip link set {} mtu 72", iface.name));
            }
            if (!exists || mtu_wrong || !interface_is_up(iface.name))
                per_iface.push_back(std::format("ip link set up {}", iface.name));
            continue;
        }
        if (!interface_exists(iface.name)) {
            warn(std::format("interface {} not found", iface.name));
            continue;
        }
        // An interface that is up is left alone, unless it should run CAN FD and does not (or the other way round).
        const bool up = interface_is_up(iface.name);
        if (up && interface_is_fd(iface.name) == iface.fd)
            continue;
        if (up) {
            if (iface.baudrate == 0) {
                warn(std::format("{} is up {} CAN FD; set a baudrate to reconfigure it", iface.name,
                                 iface.fd ? "without" : "with"));
                continue;
            }
            per_iface.push_back(std::format("ip link set down {}", iface.name));
        }
        if (iface.baudrate > 0 && iface.fd)
            per_iface.push_back(std::format("ip link set {} type can bitrate {} dbitrate {} fd on", iface.name,
                                            iface.baudrate, iface.data_baudrate > 0 ? iface.data_baudrate : 2'000'000));
        else if (iface.baudrate > 0)
            per_iface.push_back(std::format("ip link set {} type can bitrate {} fd off", iface.name, iface.baudrate));
        per_iface.push_back(std::format("ip link set up {}", iface.name));
    }
    if (need_vcan_module && !per_iface.empty())
        cmds.push_back("modprobe vcan");
    cmds.insert(cmds.end(), per_iface.begin(), per_iface.end());
    return cmds;
}

std::string run_privileged(const std::vector<std::string>& commands, bool graphical) {
    if (commands.empty()) return {};

    std::string script;
    for (const auto& c : commands) script += (script.empty() ? "" : " && ") + c;

    std::string prefix;
    if (geteuid() != 0) {
        const bool has_pkexec = std::system("command -v pkexec >/dev/null 2>&1") == 0;
        prefix = graphical && has_pkexec ? "pkexec " : "sudo ";
    }
    const std::string cmd = std::format("{}sh -c '{}'", prefix, script);
    std::println("  $ {}", cmd);
    if (std::system(cmd.c_str()) != 0)
        return std::format("Command failed: {}", script);
    return {};
}

void setup_interfaces(const Config& cfg) {
    std::vector<std::string> warnings;
    const auto cmds = plan_setup(cfg, &warnings);
    for (const auto& w : warnings)
        std::println(stderr, "Warning: {}, skipping.", w);
    if (const auto err = run_privileged(cmds, false); !err.empty())
        throw std::runtime_error(err);
}

// down + up with SIOCSIFFLAGS; errno on failure, 0 on success
static int cycle_interface(const std::string& name) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return errno;
    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);
    int err = 0;
    if (::ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        err = errno;
    } else {
        ifr.ifr_flags &= ~IFF_UP;
        if (::ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
            err = errno;
        } else {
            ifr.ifr_flags |= IFF_UP;
            if (::ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) err = errno;
        }
    }
    ::close(fd);
    return err;
}

std::string reset_interfaces(const std::vector<std::string>& names, bool allow_privileged, bool graphical) {
    std::vector<std::string> errors, privileged;
    for (const auto& name : names) {
        if (!safe_name(name) || !interface_exists(name)) {
            errors.push_back(std::format("{}: not found", name));
            continue;
        }
        const int err = cycle_interface(name);
        if (err == 0) continue;
        if ((err == EPERM || err == EACCES) && allow_privileged)
            privileged.push_back(std::format("ip link set {0} down && ip link set {0} up", name));
        else
            errors.push_back(std::format("{}: {}", name, err == EPERM || err == EACCES
                                                             ? "no permission (needs root or CAP_NET_ADMIN)"
                                                             : std::strerror(err)));
    }
    if (!privileged.empty())
        if (auto err = run_privileged(privileged, graphical); !err.empty()) errors.push_back(err);
    std::string out;
    for (const auto& e : errors) out += (out.empty() ? "" : "; ") + e;
    return out;
}
