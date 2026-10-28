#pragma once

#include "LogReader.hpp"

#include <map>
#include <string>

// Reads Vector BLF files (CAN, CAN FD; zlib or uncompressed containers,
// object header v1/v2) and replays them at the original speed.
//
// Interface names come from the "caneo-channel:<n>=<iface>" APP_TEXT objects
// written by BlfWriter; for foreign files channel n maps to the n-th
// configured interface, or "can<n-1>" (see set_channel_names()).
class BlfReader : public LogReader {
public:
    explicit BlfReader(std::string path);

    // Fallback names for BLF channels 1, 2, ... (e.g. the configured interfaces).
    void set_channel_names(std::vector<std::string> names) { fallback_names_ = std::move(names); }

    std::vector<std::string>                        scan_interfaces() const override;
    std::map<std::string, std::vector<std::string>> scan_messages() const override;
    void play(const OnFrame& on_frame, const OnSend& on_send = {},
              PlaybackController* controller = nullptr) override;

    struct RawFrame {
        uint64_t             ts_ns;       // unix time
        uint16_t             channel;     // 1-based
        uint32_t             id;
        std::vector<uint8_t> data;
    };

    // Calls fn for every frame in file order; channel names are resolved via
    // channel_name() afterwards. Throws on a malformed file.
    void read(const std::function<bool(const RawFrame&)>& fn) const;   // fn returns false to stop
    std::string channel_name(uint16_t channel) const;

private:
    std::string                              path_;
    std::vector<std::string>                 fallback_names_;
    mutable std::map<uint16_t, std::string>  channel_names_;   // from APP_TEXT
};
