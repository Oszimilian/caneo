#pragma once

#include <string>
#include <vector>

// CAN adapters found in /sys/class/net, grouped by the physical device (a PEAK PCAN-USB Pro FD shows up as
// two interfaces of one USB device).
struct CanDevice {
    std::string              id;        // stable: "usb:<vid>:<pid>:<serial>" or "dev:<sysfs name>"
    std::string              label;     // "PEAK PCAN-USB Pro FD (0c72:0011, serial 123)"
    std::vector<std::string> channels;  // interface names ordered by channel
};

struct CanInterfaceInfo {
    std::string name;
    std::string state;        // operstate
    bool        is_virtual;   // no hardware behind it (vcan, vxcan, ...)
};

std::vector<CanDevice>        detect_can_devices();       // real hardware only
std::vector<CanInterfaceInfo> detect_can_interfaces();    // everything of type CAN
