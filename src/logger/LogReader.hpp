#pragma once

#include "frame/CanFrame.hpp"
#include "playback/PlaybackController.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Common interface of the playback sources (MCAP, BLF).
//
//   on_frame   – frame for display (decoded if the format/decoder allows)
//   on_send    – raw bytes to put back on the bus; optional
//   controller – optional pause/resume + message filter
class LogReader {
public:
    using OnFrame = std::function<void(std::unique_ptr<CanFrame>)>;
    using OnSend  = std::function<void(const std::string& iface,
                                       uint32_t can_id,
                                       const std::vector<uint8_t>& payload)>;
    // Decodes a raw frame in place (sets msg name + signals). Used by formats
    // that only store raw frames (BLF).
    using Decode  = std::function<void(CanFrame&)>;

    virtual ~LogReader() = default;

    virtual std::vector<std::string>                        scan_interfaces() const = 0;
    virtual std::map<std::string, std::vector<std::string>> scan_messages() const   = 0;
    virtual void play(const OnFrame& on_frame, const OnSend& on_send = {},
                      PlaybackController* controller = nullptr) = 0;

    void set_decoder(Decode decode) { decode_ = std::move(decode); }

protected:
    Decode decode_;
};

// Picks the reader by file content ("LOGG" = BLF, otherwise MCAP).
std::unique_ptr<LogReader> open_log_reader(const std::string& path);
