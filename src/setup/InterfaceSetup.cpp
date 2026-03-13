#include "InterfaceSetup.hpp"
#include "compat/print.hpp"

#include <cstdlib>
#include <format>
#include <fstream>
#include <net/if.h>
#include <stdexcept>

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

static void run(const std::string& cmd) {
    std::println("  $ {}", cmd);
    if (std::system(cmd.c_str()) != 0)
        throw std::runtime_error(std::format("Command failed: {}", cmd));
}

void setup_interfaces(const Config& cfg) {
    if (cfg.virtual_can) {
        run("sudo modprobe vcan");
        for (const auto& iface : cfg.interfaces) {
            if (!interface_exists(iface.name))
                run(std::format("sudo ip link add dev {} type vcan", iface.name));
            if (!interface_is_up(iface.name))
                run(std::format("sudo ip link set up {}", iface.name));
        }
    } else {
        for (const auto& iface : cfg.interfaces) {
            if (!interface_exists(iface.name)) {
                std::println(stderr, "Warning: interface {} not found, skipping.", iface.name);
                continue;
            }
            if (interface_is_up(iface.name)) {
                std::println("  {} is already up, skipping.", iface.name);
                continue;
            }
            if (iface.baudrate > 0)
                run(std::format("sudo ip link set {} type can bitrate {}", iface.name, iface.baudrate));
            run(std::format("sudo ip link set up {}", iface.name));
        }
    }
}
