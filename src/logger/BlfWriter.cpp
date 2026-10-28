#include "BlfWriter.hpp"
#include "BlfFormat.hpp"

#include <chrono>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <zlib.h>

using namespace blf;

BlfWriter::BlfWriter(const std::string& path)
    : uncompressed_size_(FILE_HEADER_SIZE)
{
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_)
        throw std::runtime_error("BlfWriter: cannot open file: " + path + " (" + std::strerror(errno) + ")");
    write_header();   // placeholder, rewritten on flush/close
}

BlfWriter::~BlfWriter()
{
    close();
}

// ─── objects ────────────────────────────────────────────────────────────────

void BlfWriter::add_object(uint32_t type, const std::vector<uint8_t>& data, uint64_t ts_ns)
{
    if (!started_) {
        start_ns_ = ts_ns / 1'000'000 * 1'000'000;   // SYSTEMTIME has ms precision
        started_  = true;
    }
    stop_ns_ = std::max(stop_ns_, ts_ns);
    const uint64_t rel = ts_ns > start_ns_ ? ts_ns - start_ns_ : 0;

    const uint32_t header_size = OBJ_HEADER_BASE_SIZE + OBJ_HEADER_V1_SIZE;
    const uint32_t obj_size    = header_size + static_cast<uint32_t>(data.size());

    // base header: "LOBJ", header size, header version, object size, type
    put_bytes(buffer_, "LOBJ", 4);
    put_le<uint16_t>(buffer_, static_cast<uint16_t>(header_size));
    put_le<uint16_t>(buffer_, 1);
    put_le<uint32_t>(buffer_, obj_size);
    put_le<uint32_t>(buffer_, type);
    // v1 header: flags, client index, object version, timestamp
    put_le<uint32_t>(buffer_, TIME_ONE_NANS);
    put_le<uint16_t>(buffer_, 0);
    put_le<uint16_t>(buffer_, 0);
    put_le<uint64_t>(buffer_, rel);
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    buffer_.resize(buffer_.size() + data.size() % 4, 0);   // padding (not part of obj_size)

    ++object_count_;
    while (buffer_.size() >= MAX_CONTAINER_SIZE)
        write_container(MAX_CONTAINER_SIZE);
}

uint16_t BlfWriter::channel_for(const std::string& interface, uint64_t ts_ns)
{
    if (const auto it = channels_.find(interface); it != channels_.end())
        return it->second;
    const auto ch = static_cast<uint16_t>(channels_.size() + 1);
    channels_[interface] = ch;

    // APP_TEXT: source, reserved, text length, reserved, text
    const std::string text = std::string(CHANNEL_TAG) + std::to_string(ch) + "=" + interface;
    std::vector<uint8_t> d;
    put_le<uint32_t>(d, APPTEXT_MEASUREMENTCOMMENT);
    put_le<uint32_t>(d, 0);
    put_le<uint32_t>(d, static_cast<uint32_t>(text.size()));
    put_le<uint32_t>(d, 0);
    put_bytes(d, text.data(), text.size());
    add_object(APP_TEXT, d, ts_ns);
    return ch;
}

void BlfWriter::write(const CanFrame& frame)
{
    if (!file_)
        return;
    const uint64_t ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(frame.timestamp().time_since_epoch()).count());
    const uint16_t channel = channel_for(frame.header().interface, ts);

    uint32_t id = frame.header().id & 0x1FFFFFFF;
    if (id > 0x7FF)                 // SocketCAN strips the EFF flag
        id |= CAN_MSG_EXT;
    const auto& payload = frame.payload();

    std::vector<uint8_t> d;
    if (payload.size() <= 8) {
        // CAN_MESSAGE: channel, flags, dlc, id, data[8]
        put_le<uint16_t>(d, channel);
        put_le<uint8_t>(d, 0);
        put_le<uint8_t>(d, static_cast<uint8_t>(payload.size()));
        put_le<uint32_t>(d, id);
        uint8_t data[8] = {};
        std::memcpy(data, payload.data(), payload.size());
        put_bytes(d, data, 8);
        add_object(CAN_MESSAGE, d, ts);
    } else {
        // CAN_FD_MESSAGE: channel, flags, dlc code, id, frame length,
        // arb bit count, fd flags, valid bytes, 5 reserved, data[64]
        put_le<uint16_t>(d, channel);
        put_le<uint8_t>(d, 0);
        put_le<uint8_t>(d, len_to_dlc(payload.size()));
        put_le<uint32_t>(d, id);
        put_le<uint32_t>(d, 0);
        put_le<uint8_t>(d, 0);
        put_le<uint8_t>(d, FD_EDL | FD_BRS);
        put_le<uint8_t>(d, static_cast<uint8_t>(std::min<std::size_t>(payload.size(), 64)));
        d.resize(d.size() + 5, 0);
        uint8_t data[64] = {};
        std::memcpy(data, payload.data(), std::min<std::size_t>(payload.size(), 64));
        put_bytes(d, data, 64);
        add_object(CAN_FD_MESSAGE, d, ts);
    }
}

// ─── containers / header ────────────────────────────────────────────────────

void BlfWriter::write_container(std::size_t size)
{
    size = std::min(size, buffer_.size());
    if (size == 0)
        return;

    uLongf               comp_len = compressBound(static_cast<uLong>(size));
    std::vector<uint8_t> comp(comp_len);
    if (compress2(comp.data(), &comp_len, buffer_.data(), static_cast<uLong>(size), Z_DEFAULT_COMPRESSION) != Z_OK)
        throw std::runtime_error("BlfWriter: zlib compression failed");
    comp.resize(comp_len);

    const uint32_t obj_size = OBJ_HEADER_BASE_SIZE + LOG_CONTAINER_HEADER_SIZE + static_cast<uint32_t>(comp.size());
    std::vector<uint8_t> hdr;
    put_bytes(hdr, "LOBJ", 4);
    put_le<uint16_t>(hdr, OBJ_HEADER_BASE_SIZE);
    put_le<uint16_t>(hdr, 1);
    put_le<uint32_t>(hdr, obj_size);
    put_le<uint32_t>(hdr, LOG_CONTAINER);
    // container header: compression method, 6 reserved, uncompressed size, 4 reserved
    put_le<uint16_t>(hdr, ZLIB_DEFLATE);
    hdr.resize(hdr.size() + 6, 0);
    put_le<uint32_t>(hdr, static_cast<uint32_t>(size));
    hdr.resize(hdr.size() + 4, 0);

    std::fwrite(hdr.data(), 1, hdr.size(), file_);
    std::fwrite(comp.data(), 1, comp.size(), file_);
    static const uint8_t zeros[4] = {};
    std::fwrite(zeros, 1, obj_size % 4, file_);

    uncompressed_size_ += OBJ_HEADER_BASE_SIZE + LOG_CONTAINER_HEADER_SIZE + size;
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(size));
}

static void put_systemtime(std::vector<uint8_t>& out, uint64_t unix_ns)
{
    // python-can writes UTC; zeros mean "unknown".
    if (unix_ns == 0) {
        out.resize(out.size() + 16, 0);
        return;
    }
    const std::time_t secs = static_cast<std::time_t>(unix_ns / 1'000'000'000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    put_le<uint16_t>(out, static_cast<uint16_t>(tm.tm_year + 1900));
    put_le<uint16_t>(out, static_cast<uint16_t>(tm.tm_mon + 1));
    put_le<uint16_t>(out, static_cast<uint16_t>(tm.tm_wday));
    put_le<uint16_t>(out, static_cast<uint16_t>(tm.tm_mday));
    put_le<uint16_t>(out, static_cast<uint16_t>(tm.tm_hour));
    put_le<uint16_t>(out, static_cast<uint16_t>(tm.tm_min));
    put_le<uint16_t>(out, static_cast<uint16_t>(tm.tm_sec));
    put_le<uint16_t>(out, static_cast<uint16_t>(unix_ns / 1'000'000 % 1000));
}

void BlfWriter::write_header()
{
    const long end = std::ftell(file_);
    const uint64_t file_size = end > static_cast<long>(FILE_HEADER_SIZE) ? static_cast<uint64_t>(end) : FILE_HEADER_SIZE;

    std::vector<uint8_t> h;
    put_bytes(h, "LOGG", 4);
    put_le<uint32_t>(h, FILE_HEADER_SIZE);
    // application id, app version (3), bin log version major/minor/build/patch
    for (uint8_t b : {uint8_t(5), uint8_t(0), uint8_t(0), uint8_t(0), uint8_t(2), uint8_t(6), uint8_t(8), uint8_t(1)})
        put_le<uint8_t>(h, b);
    put_le<uint64_t>(h, file_size);
    put_le<uint64_t>(h, uncompressed_size_);
    put_le<uint32_t>(h, static_cast<uint32_t>(object_count_));
    put_le<uint32_t>(h, 0);
    put_systemtime(h, started_ ? start_ns_ : 0);
    put_systemtime(h, started_ ? stop_ns_ : 0);
    h.resize(FILE_HEADER_SIZE, 0);

    std::fseek(file_, 0, SEEK_SET);
    std::fwrite(h.data(), 1, h.size(), file_);
    std::fseek(file_, 0, SEEK_END);
}

void BlfWriter::flush()
{
    if (!file_)
        return;
    while (!buffer_.empty())
        write_container(MAX_CONTAINER_SIZE);
    write_header();
    std::fflush(file_);
}

void BlfWriter::close()
{
    if (!file_)
        return;
    flush();
    std::fclose(file_);
    file_ = nullptr;
}
