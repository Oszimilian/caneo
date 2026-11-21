#pragma once

#include "config/Config.hpp"

#include <string>
#include <vector>

// The privileged commands (without sudo) that bring the configured CAN interfaces into shape:
// - virtual interfaces (Config::virtual_can or InterfaceConfig::virtual_can): modprobe vcan, create, bring up
// - real interfaces: set the bitrate and bring them up (skipped if already up or not present)
// Empty if there is nothing to do - then no password is needed. `warnings` receives skipped interfaces.
std::vector<std::string> plan_setup(const Config& cfg, std::vector<std::string>* warnings = nullptr);

// Runs the commands in ONE privileged shell (one password prompt): pkexec when `graphical` and available,
// else sudo; nothing when already root. Returns an error text, empty on success.
std::string run_privileged(const std::vector<std::string>& commands, bool graphical);

// plan_setup() + run_privileged() for the CLI modes. Throws std::runtime_error if it fails.
void setup_interfaces(const Config& cfg);

// "Reset HW": takes each interface down and up again - restarts the CAN controller (bus-off, error counters),
// the bitrate stays. Open sockets stay bound and keep receiving afterwards.
// Direct ioctl when caneo may (root / CAP_NET_ADMIN, e.g. the Pi service); otherwise, if `allow_privileged`,
// one run_privileged() call for the rest. Unknown / missing interfaces are reported. Returns an error text, empty on success.
std::string reset_interfaces(const std::vector<std::string>& names, bool allow_privileged, bool graphical = true);
