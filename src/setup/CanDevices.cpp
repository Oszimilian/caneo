#include "CanDevices.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace {

constexpr int ARPHRD_CAN_TYPE = 280;   // <linux/if_arp.h> ARPHRD_CAN

std::string read_line(const fs::path& p)
{
    std::ifstream f(p);
    std::string   s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\r')) s.pop_back();
    return s;
}

bool is_can(const fs::path& netdev)
{
    try { return std::stoi(read_line(netdev / "type")) == ARPHRD_CAN_TYPE; }
    catch (...) { return false; }
}

}   // namespace

std::vector<CanInterfaceInfo> detect_can_interfaces()
{
    std::vector<CanInterfaceInfo> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/net", ec)) {
        if (!is_can(e.path())) continue;
        out.push_back({e.path().filename().string(), read_line(e.path() / "operstate"),
                       !fs::exists(e.path() / "device", ec)});
    }
    std::ranges::sort(out, {}, &CanInterfaceInfo::name);
    return out;
}

std::vector<CanDevice> detect_can_devices()
{
    struct Item { std::string name; int channel; };
    std::map<std::string, CanDevice>        devices;
    std::map<std::string, std::vector<Item>> items;

    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/net", ec)) {
        if (!is_can(e.path()) || !fs::exists(e.path() / "device", ec)) continue;
        const std::string name = e.path().filename().string();

        fs::path dev = fs::canonical(e.path() / "device", ec);
        if (ec) continue;

        // walk up to the USB device (has idVendor)
        fs::path usb = dev;
        while (usb.has_parent_path() && usb != usb.root_path() && !fs::exists(usb / "idVendor", ec))
            usb = usb.parent_path();

        CanDevice d;
        if (fs::exists(usb / "idVendor", ec)) {
            const std::string vid = read_line(usb / "idVendor"), pid = read_line(usb / "idProduct");
            const std::string serial = read_line(usb / "serial");
            const std::string product = read_line(usb / "product"), vendor = read_line(usb / "manufacturer");
            d.id = std::format("usb:{}:{}:{}", vid, pid, serial.empty() ? "@" + usb.filename().string() : serial);
            d.label = std::format("{}{}{} ({}:{}{})", vendor, vendor.empty() ? "" : " ", product.empty() ? "USB CAN" : product,
                                  vid, pid, serial.empty() ? ", port " + usb.filename().string() : ", serial " + serial);
        } else {
            d.id    = "dev:" + dev.filename().string();
            d.label = dev.filename().string();
        }

        int channel = 0;
        for (const char* attr : {"dev_port", "dev_id"}) {
            const std::string v = read_line(e.path() / attr);
            if (v.empty()) continue;
            try { channel = std::stoi(v, nullptr, 0); break; } catch (...) {}
        }
        devices.emplace(d.id, d);
        items[d.id].push_back({name, channel});
    }

    std::vector<CanDevice> out;
    for (auto& [id, d] : devices) {
        auto& list = items[id];
        std::ranges::sort(list, [](const Item& a, const Item& b) {
            return a.channel != b.channel ? a.channel < b.channel : a.name < b.name;
        });
        for (const auto& it : list) d.channels.push_back(it.name);
        out.push_back(std::move(d));
    }
    return out;
}
