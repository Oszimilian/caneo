#pragma once

#include "frame/CanFrame.hpp"
#include "playback/PlaybackController.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Reads an MCAP file and replays CAN frames at original speed.
//
//   on_frame   – called for each /data channel message (decoded signals → TUI)
//   on_send    – called for each /raw channel message (raw bytes → SocketCAN send)
//                optional; pass {} to skip actual bus transmission
//   controller – optional per-interface pause/resume controller; when all
//                interfaces are paused the thread blocks and timing resets on
//                resume so playback continues without a catch-up burst.
class McapReader {
public:
    explicit McapReader(const std::string& path);

    using OnFrame = std::function<void(std::unique_ptr<CanFrame>)>;
    using OnSend  = std::function<void(const std::string& iface,
                                       uint32_t can_id,
                                       const std::vector<uint8_t>& payload)>;

    // Returns the unique interface names found in /raw channels of the MCAP.
    // Use this to know which sockets to open before calling play().
    std::vector<std::string> scan_interfaces() const;

    // Returns message names per interface (from /data channels).
    std::map<std::string, std::vector<std::string>> scan_messages() const;

    void play(const OnFrame& on_frame, const OnSend& on_send = {},
              PlaybackController* controller = nullptr);

private:
    std::string path_;
};
