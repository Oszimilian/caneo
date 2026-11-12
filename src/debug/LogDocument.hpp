#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <format>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// A recorded log (MCAP or BLF) loaded completely into memory as time series,
// for the offline "Debug" view.
//
//   MCAP: signals are stored decoded in the file -> no DBC needed.
//   BLF : only raw frames are stored -> they are decoded with one DBC per
//         interface; decode() can be repeated with other DBCs.
//
// open()/decode() run on a worker thread; the render thread calls poll()
// each frame and reads data() when it returns true / while !busy().
class LogDocument {
public:
    enum class Format { None, Mcap, Blf };

    struct Series {
        std::string         iface, msg, signal, unit;
        std::string         label;   // "iface/msg/signal"
        std::vector<double> t;       // seconds since the first sample
        std::vector<double> v;
    };

    struct RawFrame {
        double               t;
        uint16_t             channel;
        uint32_t             id;
        std::vector<uint8_t> data;
    };

    // A CAN ID of a BLF interface that no DBC message was found for.
    struct Unmatched {
        std::string iface;
        uint32_t    id = 0;
        uint64_t    count = 0;
        std::size_t dlc = 0;
        bool        has_dbc = false;   // false: the interface has no (valid) DBC at all
        double      first_t = 0, last_t = 0;
        std::vector<uint8_t> last_data;
    };

    struct Data {
        std::string                          path;
        Format                               format = Format::None;
        std::vector<std::string>             interfaces;     // as seen in the file
        std::map<std::string, uint64_t>      frames;         // per interface
        std::map<std::string, std::string>   dbc_errors;     // per interface
        std::vector<Series>                  series;         // sorted by label, then flow results
        std::size_t                          base_series = 0; // series from the log itself
        std::vector<Unmatched>               unmatched;      // BLF only
        double                               duration = 0.0; // seconds
        std::shared_ptr<const std::vector<RawFrame>>     raws;      // BLF only
        std::map<uint16_t, std::string>                  channels;  // BLF channel -> interface
    };

    ~LogDocument();

    // Loads `path` (format detected from the content). dbcs: interface -> DBC
    // file (BLF only). fallback_names: names for BLF channels 1, 2, ...
    void open(std::string path, std::vector<std::string> fallback_names,
              std::map<std::string, std::string> dbcs);

    // Re-decodes the raw frames of a loaded BLF with other DBCs.
    void decode(std::map<std::string, std::string> dbcs);

    bool        busy() const { return busy_; }
    std::string status() const;   // progress / last error, for the UI
    double      fraction() const { return fraction_; }   // 0..1 of the current stage while busy
    // True once after a worker finished and data() changed.
    bool        poll();
    const Data& data() const { return data_; }
    bool        loaded() const { return data_.format != Format::None; }

    // Replaces the flow results appended after the log's own series (render thread, not while busy).
    // Indices of the log's own series stay valid.
    void set_extra_series(std::vector<Series> extra);

    friend std::ostream& operator<<(std::ostream& os, const LogDocument& doc);

private:
    void finish(Data&& d, std::string error);

    Data                 data_;
    mutable std::mutex   mutex_;
    std::optional<Data>  pending_;
    std::string          status_;
    std::atomic<bool>    busy_{false};
    std::atomic<uint64_t> progress_{0};
    std::atomic<double>   fraction_{0.0};
    std::atomic<bool>     decoding_{false};   // BLF stage 2 (DBC decoding) instead of reading
    std::jthread         worker_;
};

template <>
struct std::formatter<LogDocument> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const LogDocument& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
