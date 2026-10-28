#pragma once

#include "frame/CanFrame.hpp"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

// Writes Vector BLF (Binary Logging Format) files: raw CAN / CAN FD frames,
// zlib-compressed in 128 KiB log containers. Readable by CANalyzer/CANoe,
// asammdf, python-can and caneo --playback.
//
// Layout follows python-can's BLFWriter (the de-facto open reference):
//   file header "LOGG" (144 bytes, rewritten on flush/close)
//   LOBJ log containers (type 10) holding the objects:
//     CAN_MESSAGE (1) for classic frames, CAN_FD_MESSAGE (100) for > 8 bytes,
//     APP_TEXT (65) "caneo-channel:<n>=<interface>" when a new interface
//     appears, so playback can map BLF channel numbers back to interfaces.
//
// Not thread-safe; LogController serialises access.
class BlfWriter {
public:
    explicit BlfWriter(const std::string& path);   // throws std::runtime_error
    ~BlfWriter();

    BlfWriter(const BlfWriter&)            = delete;
    BlfWriter& operator=(const BlfWriter&) = delete;

    void write(const CanFrame& frame);

    // Writes all buffered objects and an up-to-date header to disk.
    void flush();
    void close();

    // Text prefix of the APP_TEXT objects holding the channel mapping.
    static constexpr const char* CHANNEL_TAG = "caneo-channel:";

private:
    uint16_t channel_for(const std::string& interface, uint64_t ts_ns);
    void     add_object(uint32_t type, const std::vector<uint8_t>& data, uint64_t ts_ns);
    void     write_container(std::size_t size);
    void     write_header();

    std::FILE*                        file_ = nullptr;
    std::vector<uint8_t>              buffer_;           // uncompressed objects
    uint64_t                          object_count_ = 0;
    uint64_t                          uncompressed_size_;
    uint64_t                          start_ns_ = 0;     // unix ns, ms precision
    uint64_t                          stop_ns_  = 0;
    bool                              started_  = false;
    std::map<std::string, uint16_t>   channels_;         // interface -> BLF channel (1-based)
};
