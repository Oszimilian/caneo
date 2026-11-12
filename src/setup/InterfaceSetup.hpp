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
