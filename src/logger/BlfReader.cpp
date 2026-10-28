#include "BlfReader.hpp"

#include "BlfFormat.hpp"
#include "BlfWriter.hpp"
#include "McapReader.hpp"
#include "compat/print.hpp"

#include <chrono>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <set>
#include <stdexcept>
#include <thread>
#include <zlib.h>

using namespace blf;

// ─── reader selection ───────────────────────────────────────────────────────

std::unique_ptr<LogReader> open_log_reader(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot open " + path);
    char magic[4] = {};
    f.read(magic, 4);
    if (std::memcmp(magic, "LOGG", 4) == 0)
        return std::make_unique<BlfReader>(path);
    return std::make_unique<McapReader>(path);
}

// ─── parsing ────────────────────────────────────────────────────────────────

BlfReader::BlfReader(std::string path) : path_(std::move(path)) {}

static uint64_t systemtime_to_ns(const uint8_t* p)
{
    std::tm tm{};
    tm.tm_year = get_le<uint16_t>(p + 0) - 1900;
    tm.tm_mon  = get_le<uint16_t>(p + 2) - 1;
    tm.tm_mday = get_le<uint16_t>(p + 6);
    tm.tm_hour = get_le<uint16_t>(p + 8);
    tm.tm_min  = get_le<uint16_t>(p + 10);
    tm.tm_sec  = get_le<uint16_t>(p + 12);
    const uint16_t ms = get_le<uint16_t>(p + 14);
    if (tm.tm_year < 70)
        return 0;   // unset
    const std::time_t secs = timegm(&tm);
    return static_cast<uint64_t>(secs) * 1'000'000'000ULL + ms * 1'000'000ULL;
}

std::string BlfReader::channel_name(uint16_t channel) const
{
    if (const auto it = channel_names_.find(channel); it != channel_names_.end())
        return it->second;
    if (channel >= 1 && channel <= fallback_names_.size())
        return fallback_names_[channel - 1];
    return std::format("can{}", channel > 0 ? channel - 1 : 0);
}

void BlfReader::read(const std::function<bool(const RawFrame&)>& fn) const
{
    std::ifstream f(path_, std::ios::binary);
    if (!f)
        throw std::runtime_error("BlfReader: cannot open " + path_);

    uint8_t header[FILE_HEADER_SIZE];
    if (!f.read(reinterpret_cast<char*>(header), 72) || std::memcmp(header, "LOGG", 4) != 0)
        throw std::runtime_error("BlfReader: not a BLF file: " + path_);
    const uint32_t header_size = get_le<uint32_t>(header + 4);
    const uint64_t start_ns    = systemtime_to_ns(header + 40);
    f.seekg(header_size, std::ios::beg);

    // Decompressed object stream; objects may continue in the next container.
    std::vector<uint8_t> stream;
    std::size_t          pos = 0;

    // Parses as many complete objects from `stream` as possible.
    // Returns false if fn asked to stop.
    auto parse_stream = [&]() -> bool {
        while (true) {
            // Objects are 4-byte aligned, but some writers pad differently:
            // search the next signature within a few bytes like python-can.
            std::size_t sig = pos;
            while (sig + 4 <= stream.size() && sig < pos + 8 && std::memcmp(&stream[sig], "LOBJ", 4) != 0)
                ++sig;
            if (sig + OBJ_HEADER_BASE_SIZE > stream.size())
                return true;                          // need more data
            if (std::memcmp(&stream[sig], "LOBJ", 4) != 0)
                throw std::runtime_error("BlfReader: corrupt object stream");

            const uint8_t* p           = &stream[sig];
            const uint16_t hdr_size    = get_le<uint16_t>(p + 4);
            const uint16_t hdr_version = get_le<uint16_t>(p + 6);
            const uint32_t obj_size    = get_le<uint32_t>(p + 8);
            const uint32_t obj_type    = get_le<uint32_t>(p + 12);
            if (obj_size < OBJ_HEADER_BASE_SIZE)
                throw std::runtime_error("BlfReader: invalid object size");
            if (sig + obj_size > stream.size())
                return true;                          // continues in next container
            pos = sig + obj_size;

            uint32_t flags = 0;
            uint64_t ts    = 0;
            if (hdr_version == 1) {
                flags = get_le<uint32_t>(p + 16);
                ts    = get_le<uint64_t>(p + 24);
            } else if (hdr_version == 2) {
                flags = get_le<uint32_t>(p + 16);
                ts    = get_le<uint64_t>(p + 24);
            } else {
                continue;
            }
            const uint64_t ts_ns = start_ns + (flags == TIME_TEN_MICS ? ts * 10'000 : ts);
            const uint8_t* d     = p + hdr_size;
            const std::size_t dl = obj_size - hdr_size;

            RawFrame fr;
            fr.ts_ns = ts_ns;
            switch (obj_type) {
                case CAN_MESSAGE:
                case CAN_MESSAGE2: {
                    if (dl < 16) continue;
                    fr.channel = get_le<uint16_t>(d);
                    const uint8_t dlc = d[3];
                    fr.id = get_le<uint32_t>(d + 4) & 0x1FFFFFFF;
                    fr.data.assign(d + 8, d + 8 + std::min<std::size_t>(dlc, 8));
                    break;
                }
                case CAN_FD_MESSAGE: {
                    if (dl < 84) continue;
                    fr.channel = get_le<uint16_t>(d);
                    fr.id      = get_le<uint32_t>(d + 4) & 0x1FFFFFFF;
                    const uint8_t valid = std::min<uint8_t>(d[14], 64);
                    fr.data.assign(d + 20, d + 20 + valid);
                    break;
                }
                case CAN_FD_MESSAGE_64: {
                    if (dl < 40) continue;
                    fr.channel = d[0];
                    const uint8_t valid    = d[2];
                    fr.id                  = get_le<uint32_t>(d + 4) & 0x1FFFFFFF;
                    const uint8_t ext_off  = d[35];
                    const std::size_t room = (ext_off ? ext_off : obj_size) - hdr_size - 40;
                    const std::size_t n    = std::min<std::size_t>({valid, room, dl - 40, 64});
                    fr.data.assign(d + 40, d + 40 + n);
                    fr.data.resize(valid, 0);
                    break;
                }
                case APP_TEXT: {
                    if (dl < 16) continue;
                    const uint32_t len = std::min<uint32_t>(get_le<uint32_t>(d + 8), static_cast<uint32_t>(dl - 16));
                    const std::string text(reinterpret_cast<const char*>(d + 16), len);
                    // "caneo-channel:<n>=<iface>"
                    if (text.starts_with(BlfWriter::CHANNEL_TAG)) {
                        const auto eq = text.find('=');
                        if (eq != std::string::npos) {
                            try {
                                const int ch = std::stoi(text.substr(std::strlen(BlfWriter::CHANNEL_TAG)));
                                channel_names_[static_cast<uint16_t>(ch)] = text.substr(eq + 1);
                            } catch (...) {}
                        }
                    }
                    continue;
                }
                default:
                    continue;
            }
            if (!fn(fr))
                return false;
        }
    };

    std::vector<uint8_t> obj;
    while (true) {
        uint8_t base[OBJ_HEADER_BASE_SIZE];
        if (!f.read(reinterpret_cast<char*>(base), sizeof(base)))
            break;                                   // EOF (also: truncated file after a crash)
        if (std::memcmp(base, "LOBJ", 4) != 0)
            break;                                   // trailing garbage / unfinished write
        const uint32_t obj_size = get_le<uint32_t>(base + 8);
        const uint32_t obj_type = get_le<uint32_t>(base + 12);
        if (obj_size < OBJ_HEADER_BASE_SIZE)
            break;
        obj.resize(obj_size - OBJ_HEADER_BASE_SIZE);
        if (!f.read(reinterpret_cast<char*>(obj.data()), static_cast<std::streamsize>(obj.size())))
            break;
        f.ignore(obj_size % 4);

        // drop consumed bytes before appending
        if (pos > 0) {
            stream.erase(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(pos));
            pos = 0;
        }

        if (obj_type == LOG_CONTAINER) {
            if (obj.size() < LOG_CONTAINER_HEADER_SIZE) continue;
            const uint16_t method = get_le<uint16_t>(obj.data());
            const uint32_t usize  = get_le<uint32_t>(obj.data() + 8);
            const uint8_t* cdata  = obj.data() + LOG_CONTAINER_HEADER_SIZE;
            const std::size_t clen = obj.size() - LOG_CONTAINER_HEADER_SIZE;
            if (method == NO_COMPRESSION) {
                stream.insert(stream.end(), cdata, cdata + clen);
            } else if (method == ZLIB_DEFLATE) {
                const std::size_t old = stream.size();
                stream.resize(old + usize);
                uLongf out_len = usize;
                if (uncompress(stream.data() + old, &out_len, cdata, static_cast<uLong>(clen)) != Z_OK)
                    throw std::runtime_error("BlfReader: zlib error in " + path_);
                stream.resize(old + out_len);
            } else {
                continue;                            // unknown compression
            }
        } else {
            // Object outside a container: feed it as-is.
            stream.insert(stream.end(), base, base + sizeof(base));
            stream.insert(stream.end(), obj.begin(), obj.end());
            stream.resize(stream.size() + obj_size % 4, 0);
        }
        if (!parse_stream())
            return;
    }
}

// ─── LogReader interface ────────────────────────────────────────────────────

std::vector<std::string> BlfReader::scan_interfaces() const
{
    std::set<uint16_t> channels;
    read([&](const RawFrame& fr) { channels.insert(fr.channel); return true; });
    std::vector<std::string> out;
    for (const uint16_t ch : channels)
        out.push_back(channel_name(ch));
    return out;
}

std::map<std::string, std::vector<std::string>> BlfReader::scan_messages() const
{
    std::set<std::pair<uint16_t, uint32_t>> ids;
    read([&](const RawFrame& fr) { ids.insert({fr.channel, fr.id}); return true; });

    std::map<std::string, std::set<std::string>> names;
    for (const auto& [ch, id] : ids) {
        CanFrame frame(CanHeader{id, channel_name(ch), 0}, {});
        if (decode_) {
            try { decode_(frame); } catch (...) {}
        }
        names[channel_name(ch)].insert(frame.msg_name().empty() ? std::format("0x{:03X}", id)
                                                                : frame.msg_name());
    }
    std::map<std::string, std::vector<std::string>> out;
    for (auto& [iface, set] : names)
        out[iface] = {set.begin(), set.end()};
    return out;
}

void BlfReader::play(const OnFrame& on_frame, const OnSend& on_send, PlaybackController* controller)
{
    bool     started = false;
    uint64_t t0_ns   = 0;
    std::chrono::steady_clock::time_point start_time;
    uint64_t frames  = 0;

    read([&](const RawFrame& fr) -> bool {
        // All interfaces paused: block, then restart the timing reference so
        // there is no catch-up burst (same behaviour as McapReader).
        if (controller && !controller->any_running()) {
            controller->wait_for_any_running();
            if (controller->is_stopped()) return false;
            started = false;
        }
        if (!started) {
            started    = true;
            t0_ns      = fr.ts_ns;
            start_time = std::chrono::steady_clock::now();
        }
        const auto target = start_time + std::chrono::nanoseconds(fr.ts_ns >= t0_ns ? fr.ts_ns - t0_ns : 0);
        if (controller) {
            if (controller->sleep_until(target)) return false;
        } else {
            std::this_thread::sleep_until(target);
        }

        const std::string iface = channel_name(fr.channel);
        if (controller && !controller->is_running(iface))
            return true;

        auto frame = std::make_unique<CanFrame>(
            CanHeader{fr.id, iface, static_cast<uint8_t>(fr.data.size())}, fr.data);
        if (decode_) {
            try { decode_(*frame); } catch (...) {}
        }
        const std::string name = frame->msg_name().empty() ? std::format("0x{:03X}", fr.id) : frame->msg_name();
        if (controller && !controller->is_message_enabled(iface, name))
            return true;

        if (on_send)  on_send(iface, fr.id, fr.data);
        if (on_frame) on_frame(std::move(frame));
        ++frames;
        return true;
    });
    std::println("BlfReader: playback finished ({} frames).", frames);
}
