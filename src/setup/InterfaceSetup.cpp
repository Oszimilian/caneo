#include "InterfaceSetup.hpp"
#include "compat/print.hpp"

#include <cstdlib>
#include <format>
#include <net/if.h>
#include <stdexcept>

static bool interface_exists(const std::string& name) {
    return if_nametoindex(name.c_str()) != 0;
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
            run(std::format("sudo ip link set up {}", iface.name));
        }
    } else {
        for (const auto& iface : cfg.interfaces) {
            if (iface.baudrate > 0)
                run(std::format("sudo ip link set {} type can bitrate {}", iface.name, iface.baudrate));
            run(std::format("sudo ip link set up {}", iface.name));
        }
    }
}
