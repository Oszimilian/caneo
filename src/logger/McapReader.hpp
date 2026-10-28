#pragma once

#include "LogReader.hpp"

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
class McapReader : public LogReader {
public:
    explicit McapReader(const std::string& path);

    // Returns the unique interface names found in /raw channels of the MCAP.
    // Use this to know which sockets to open before calling play().
    std::vector<std::string> scan_interfaces() const override;

    // Returns message names per interface (from /data channels).
    std::map<std::string, std::vector<std::string>> scan_messages() const override;

    void play(const OnFrame& on_frame, const OnSend& on_send = {},
              PlaybackController* controller = nullptr) override;

    // Unpaced read of the decoded (/data) channels, in time order.
    // fn(log_time_ns, interface, message name, signals) returns false to stop.
    void read_signals(const std::function<bool(uint64_t, const std::string&, const std::string&,
                                               const std::vector<DecodedSignal>&)>& fn) const;

private:
    std::string path_;
};
