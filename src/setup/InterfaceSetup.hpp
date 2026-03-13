#pragma once

#include "config/Config.hpp"

// Sets up CAN interfaces based on the config.
// - virtual_can=true:  loads the vcan kernel module, creates missing vcan
//                      interfaces, and brings them up.
// - virtual_can=false: configures each real CAN interface with its baudrate
//                      and brings it up.
// Throws std::runtime_error if a required system command fails.
void setup_interfaces(const Config& cfg);
