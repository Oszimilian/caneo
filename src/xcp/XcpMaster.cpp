#include "XcpMaster.hpp"

#include "XcpCanTransport.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <ctime>
#include <limits>
#include <stdexcept>

namespace {

// command codes (ASAM XCP part 2)
enum : uint8_t {
    CC_CONNECT                 = 0xFF,
    CC_DISCONNECT              = 0xFE,
    CC_GET_STATUS              = 0xFD,
    CC_SYNCH                   = 0xFC,
    CC_GET_ID                  = 0xFA,
    CC_SET_MTA                 = 0xF6,
    CC_UPLOAD                  = 0xF5,
    CC_SHORT_UPLOAD            = 0xF4,
    CC_DOWNLOAD                = 0xF0,
    CC_SET_DAQ_PTR             = 0xE2,
    CC_WRITE_DAQ               = 0xE1,
    CC_SET_DAQ_LIST_MODE       = 0xE0,
    CC_START_STOP_DAQ_LIST     = 0xDE,
    CC_START_STOP_SYNCH        = 0xDD,
    CC_GET_DAQ_PROCESSOR_INFO  = 0xDA,
    CC_GET_DAQ_RESOLUTION_INFO = 0xD9,
    CC_GET_DAQ_EVENT_INFO      = 0xD7,
    CC_FREE_DAQ                = 0xD6,
    CC_ALLOC_DAQ               = 0xD5,
    CC_ALLOC_ODT               = 0xD4,
    CC_ALLOC_ODT_ENTRY         = 0xD3,
};
// packet identifiers slave -> master
enum : uint8_t { PID_RES = 0xFF, PID_ERR = 0xFE, PID_EV = 0xFD, PID_SERV = 0xFC };

constexpr uint8_t ERR_CMD_SYNCH   = 0x00;
constexpr uint8_t ERR_CMD_UNKNOWN = 0x20;

struct XcpError : std::runtime_error {
    uint8_t code;
    XcpError(uint8_t cmd, uint8_t c)
        : std::runtime_error(std::format("command 0x{:02X}: {}", cmd, xcp_error_name(c))), code(c) {}
};

struct XcpTimeout : std::runtime_error {
    explicit XcpTimeout(uint8_t cmd) : std::runtime_error(std::format("command 0x{:02X}: no response (timeout)", cmd)) {}
};

// unit code of TIMESTAMP_MODE / TIME_UNIT -> seconds
double unit_seconds(int unit)
{
    switch (unit) {
        case 0:  return 1e-9;
        case 1:  return 1e-8;
        case 2:  return 1e-7;
        case 3:  return 1e-6;
        case 4:  return 1e-5;
        case 5:  return 1e-4;
        case 6:  return 1e-3;
        case 7:  return 1e-2;
        case 8:  return 1e-1;
        case 9:  return 1.0;
        case 10: return 1e-12;
        case 11: return 1e-11;
        case 12: return 1e-10;
        default: return 1e-6;
    }
}

std::string clock_string()
{
    const std::time_t now = std::time(nullptr);
    std::tm           tm{};
    localtime_r(&now, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

}   // namespace

// ── helpers ──────────────────────────────────────────────────────────────────

const char* to_string(XcpMaster::State s)
{
    switch (s) {
        case XcpMaster::State::Disconnected: return "disconnected";
        case XcpMaster::State::Connecting:   return "connecting";
        case XcpMaster::State::Connected:    return "connected";
        case XcpMaster::State::Error:        return "error";
    }
    return "?";
}

std::string xcp_error_name(uint8_t code)
{
    switch (code) {
        case 0x00: return "ERR_CMD_SYNCH";
        case 0x10: return "ERR_CMD_BUSY";
        case 0x11: return "ERR_DAQ_ACTIVE";
        case 0x12: return "ERR_PGM_ACTIVE";
        case 0x20: return "ERR_CMD_UNKNOWN (not supported by the slave)";
        case 0x21: return "ERR_CMD_SYNTAX";
        case 0x22: return "ERR_OUT_OF_RANGE";
        case 0x23: return "ERR_WRITE_PROTECTED";
        case 0x24: return "ERR_ACCESS_DENIED";
        case 0x25: return "ERR_ACCESS_LOCKED (seed & key needed)";
        case 0x26: return "ERR_PAGE_NOT_VALID";
        case 0x27: return "ERR_MODE_NOT_VALID";
        case 0x28: return "ERR_SEGMENT_NOT_VALID";
        case 0x29: return "ERR_SEQUENCE";
        case 0x2A: return "ERR_DAQ_CONFIG";
        case 0x30: return "ERR_MEMORY_OVERFLOW";
        case 0x31: return "ERR_GENERIC";
        case 0x32: return "ERR_VERIFY";
        case 0x33: return "ERR_RESOURCE_TEMPORARY_NOT_ACCESSIBLE";
        default:   return std::format("error 0x{:02X}", code);
    }
}

XcpVar XcpVar::from(const ElfVariable& v, uint8_t ext, int event)
{
    XcpVar x;
    x.name       = v.path;
    x.address    = v.address;
    x.ext        = ext;
    x.size       = v.size ? v.size : xcp_type_size(v.type);
    x.type       = v.type;
    x.bit_offset = v.bit_offset;
    x.bit_size   = v.bit_size;
    x.event      = event;
    x.conv       = v.conv;
    return x;
}

static uint64_t read_raw(const uint8_t* p, std::size_t n, bool big)
{
    uint64_t raw = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t idx = big ? i : n - 1 - i;
        raw = (raw << 8) | p[idx];
    }
    return raw;
}

static void write_raw(uint8_t* p, std::size_t n, uint64_t raw, bool big)
{
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t idx = big ? n - 1 - i : i;
        p[idx] = static_cast<uint8_t>(raw >> (8 * i));
    }
}

static int64_t sign_extend(uint64_t raw, unsigned bits)
{
    if (bits >= 64) return static_cast<int64_t>(raw);
    const uint64_t m = uint64_t{1} << (bits - 1);
    raw &= (uint64_t{1} << bits) - 1;
    return static_cast<int64_t>((raw ^ m) - m);
}

static double decode_raw(const uint8_t* p, const XcpVar& v, bool big_endian)
{
    const std::size_t n   = std::min<std::size_t>(v.size ? v.size : xcp_type_size(v.type), 8);
    uint64_t          raw = read_raw(p, n, big_endian);
    if (v.bit_size) {
        raw >>= v.bit_offset;
        if (v.bit_size < 64) raw &= (uint64_t{1} << v.bit_size) - 1;
        if (xcp_type_signed(v.type)) return static_cast<double>(sign_extend(raw, v.bit_size));
        return static_cast<double>(raw);
    }
    switch (v.type) {
        case XcpType::F32:  return std::bit_cast<float>(static_cast<uint32_t>(raw));
        case XcpType::F64:  return std::bit_cast<double>(raw);
        case XcpType::Bool: return raw != 0 ? 1.0 : 0.0;
        case XcpType::I8: case XcpType::I16: case XcpType::I32: case XcpType::I64:
            return static_cast<double>(sign_extend(raw, static_cast<unsigned>(8 * n)));
        default:
            return static_cast<double>(raw);
    }
}

double xcp_decode(const uint8_t* p, const XcpVar& v, bool big_endian)
{
    return v.conv.to_phys(decode_raw(p, v, big_endian));
}

std::vector<uint8_t> xcp_encode(double phys, const XcpVar& v, bool big_endian, const std::vector<uint8_t>& current)
{
    const double      value = v.conv.to_raw(phys);
    const std::string what  = v.conv.identity() ? std::format("{}", value) : std::format("{} (raw {})", phys, value);
    const std::size_t n = std::min<std::size_t>(v.size ? v.size : xcp_type_size(v.type), 8);
    std::vector<uint8_t> out(n, 0);
    if (!std::isfinite(value) && !xcp_type_float(v.type)) throw std::runtime_error("value is not a number");

    // integer value clamped to the range of `bits`
    const auto to_int = [&](unsigned bits, bool sgn) -> uint64_t {
        const double r = std::round(value);
        if (sgn) {
            const double lo = -std::ldexp(1.0, static_cast<int>(bits) - 1), hi = std::ldexp(1.0, static_cast<int>(bits) - 1) - 1;
            if (r < lo || r > hi) throw std::runtime_error(std::format("{} is out of range [{}, {}]", what, lo, hi));
            return static_cast<uint64_t>(static_cast<int64_t>(r));
        }
        const double hi = std::ldexp(1.0, static_cast<int>(bits)) - 1;
        if (r < 0 || r > hi) throw std::runtime_error(std::format("{} is out of range [0, {}]", what, hi));
        return r >= 18446744073709551615.0 ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(r);
    };

    if (v.bit_size) {
        if (current.size() < n) throw std::runtime_error("bitfield: current value missing");
        uint64_t storage = read_raw(current.data(), n, big_endian);
        const uint64_t mask = (v.bit_size < 64 ? (uint64_t{1} << v.bit_size) - 1 : ~uint64_t{0}) << v.bit_offset;
        const uint64_t bits = to_int(v.bit_size, xcp_type_signed(v.type)) << v.bit_offset;
        storage = (storage & ~mask) | (bits & mask);
        write_raw(out.data(), n, storage, big_endian);
        return out;
    }
    uint64_t raw = 0;
    switch (v.type) {
        case XcpType::F32:  raw = std::bit_cast<uint32_t>(static_cast<float>(value)); break;
        case XcpType::F64:  raw = std::bit_cast<uint64_t>(value); break;
        case XcpType::Bool: raw = value != 0.0 ? 1 : 0; break;
        default:            raw = to_int(static_cast<unsigned>(8 * n), xcp_type_signed(v.type)); break;
    }
    write_raw(out.data(), n, raw, big_endian);
    return out;
}

// ── master ───────────────────────────────────────────────────────────────────

XcpMaster::XcpMaster(XcpTargetConfig target)
    : target_(std::move(target))
    , thread_([this](std::stop_token st) { run(st); })
{}

XcpMaster::~XcpMaster()
{
    post(Disconnect{});
    thread_.request_stop();
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void XcpMaster::set_sample_fn(SampleFn fn)
{
    sample_fn_ = std::move(fn);
}

void XcpMaster::post(Request r)
{
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(r));
    }
    cv_.notify_all();
}

void XcpMaster::connect()    { post(Connect{}); }
void XcpMaster::disconnect() { post(Disconnect{}); }
void XcpMaster::set_measurements(std::vector<XcpVar> vars, int poll_ms) { post(Measure{std::move(vars), poll_ms}); }
void XcpMaster::write(XcpVar var, double value) { post(Write{std::move(var), value}); }
void XcpMaster::read(XcpVar var) { post(Read{std::move(var)}); }

XcpMaster::State XcpMaster::state() const { std::lock_guard lock(mutex_); return state_; }
std::string XcpMaster::status() const { std::lock_guard lock(mutex_); return status_; }
XcpSlaveInfo XcpMaster::info() const { std::lock_guard lock(mutex_); return info_; }
std::vector<XcpEventInfo> XcpMaster::events() const { std::lock_guard lock(mutex_); return events_; }

std::vector<std::string> XcpMaster::take_messages()
{
    std::lock_guard lock(mutex_);
    return std::exchange(messages_, {});
}

void XcpMaster::set_state(State s, std::string msg)
{
    std::lock_guard lock(mutex_);
    state_  = s;
    status_ = std::move(msg);
}

void XcpMaster::message(std::string m)
{
    std::lock_guard lock(mutex_);
    messages_.push_back(clock_string() + "  " + std::move(m));
    if (messages_.size() > 500) messages_.erase(messages_.begin(), messages_.begin() + 100);
}

double XcpMaster::now() const
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
}

void XcpMaster::emit(const std::string& name, double t, double v)
{
    ++samples_;
    if (sample_fn_) sample_fn_(name, t, v);
}

void XcpMaster::put16(std::vector<uint8_t>& b, uint16_t v) const
{
    if (info_.big_endian) { b.push_back(static_cast<uint8_t>(v >> 8)); b.push_back(static_cast<uint8_t>(v)); }
    else                  { b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(v >> 8)); }
}

void XcpMaster::put32(std::vector<uint8_t>& b, uint32_t v) const
{
    for (int i = 0; i < 4; ++i) {
        const int sh = info_.big_endian ? 24 - 8 * i : 8 * i;
        b.push_back(static_cast<uint8_t>(v >> sh));
    }
}

uint16_t XcpMaster::get16(const uint8_t* p) const
{
    return static_cast<uint16_t>(read_raw(p, 2, info_.big_endian));
}

uint32_t XcpMaster::get32(const uint8_t* p) const
{
    return static_cast<uint32_t>(read_raw(p, 4, info_.big_endian));
}

// ── worker ───────────────────────────────────────────────────────────────────

void XcpMaster::run(std::stop_token st)
{
    while (!st.stop_requested()) {
        std::deque<Request> todo;
        {
            std::unique_lock lock(mutex_);
            if (!transport_)
                cv_.wait(lock, [&] { return !queue_.empty() || st.stop_requested(); });
            todo.swap(queue_);
        }
        for (auto& r : todo) handle(r);
        if (st.stop_requested()) break;
        if (!transport_) continue;

        try {
            if (daq_dirty_) setup_daq();
            const auto t = std::chrono::steady_clock::now();
            const bool has_poll = std::ranges::any_of(vars_, [](const XcpVar& v) { return v.event < 0; });
            if (has_poll && t >= next_poll_) {
                next_poll_ = std::max(next_poll_ + std::chrono::milliseconds(poll_ms_), t);
                poll_once();
                if (state() == State::Error) set_state(State::Connected, "connected (the slave responds again)");
            }
            auto wait = std::chrono::microseconds(10'000);
            if (has_poll)
                wait = std::clamp(std::chrono::duration_cast<std::chrono::microseconds>(next_poll_ - std::chrono::steady_clock::now()),
                                  std::chrono::microseconds(0), wait);
            pump(wait);

            const auto rt = std::chrono::steady_clock::now();
            if (rt - rate_t_ >= std::chrono::seconds(1)) {
                rate_hz_ = static_cast<double>(samples_) / std::chrono::duration<double>(rt - rate_t_).count();
                samples_ = 0;
                rate_t_  = rt;
            }
        } catch (const XcpTimeout& e) {
            if (++timeouts_ >= 5) {
                set_state(State::Error, "slave does not respond - check IDs, baudrate and that the ECU runs");
                timeouts_ = 0;
            }
            message(e.what());
        } catch (const std::exception& e) {
            message(std::string("error: ") + e.what());
            do_disconnect(false);
            set_state(State::Error, e.what());
        }
    }
    do_disconnect(transport_ != nullptr);
}

void XcpMaster::handle(Request& r)
{
    try {
        std::visit([&](auto& req) {
            using T = std::decay_t<decltype(req)>;
            if constexpr (std::is_same_v<T, Connect>) {
                do_connect();
            } else if constexpr (std::is_same_v<T, Disconnect>) {
                if (transport_) {
                    do_disconnect(true);
                    set_state(State::Disconnected, "disconnected");
                }
            } else if constexpr (std::is_same_v<T, Measure>) {
                vars_      = std::move(req.vars);
                poll_ms_   = std::max(1, req.poll_ms);
                daq_dirty_ = true;
            } else if constexpr (std::is_same_v<T, Write>) {
                if (!transport_) throw std::runtime_error("not connected");
                do_write(req.var, req.value);
            } else if constexpr (std::is_same_v<T, Read>) {
                if (!transport_) throw std::runtime_error("not connected");
                do_read(req.var);
            }
        }, r);
    } catch (const std::exception& e) {
        message(std::string("error: ") + e.what());
        if (std::holds_alternative<Connect>(r)) {
            do_disconnect(false);
            set_state(State::Error, e.what());
        }
    }
}

void XcpMaster::do_connect()
{
    if (transport_) do_disconnect(true);
    set_state(State::Connecting, "connecting...");
    transport_ = std::make_unique<XcpCanTransport>(target_.interface, target_.tx_id, target_.rx_id, target_.fd, target_.pad);
    t0_ = rate_t_ = std::chrono::steady_clock::now();
    next_poll_   = t0_;
    ts_anchored_ = false;
    timeouts_    = 0;
    samples_     = 0;
    daq_.clear();
    pid_map_.clear();

    XcpSlaveInfo info;
    {
        std::lock_guard lock(mutex_);
        info_ = {};   // byte order little until CONNECT told us
    }
    std::vector<uint8_t> res;
    try {
        res = command({CC_CONNECT, 0x00});
    } catch (const XcpTimeout&) {
        // Some slaves only accept a CONNECT of exactly 2 bytes (XCPlite with XCP_ENABLE_PROTOCOL_LAYER_ETH):
        // try once more without padding before giving up.
        if (!transport_->pad()) throw;
        transport_->set_pad(false);
        res = command({CC_CONNECT, 0x00});
        message("no response to a padded CONNECT - padding switched off (uncheck 'pad frames' in the XCP block)");
    }
    if (res.size() < 8) throw std::runtime_error("CONNECT: short response");
    info.resource    = res[1];
    const uint8_t cm = res[2];
    info.big_endian  = cm & 0x01;
    info.granularity = 1 << ((cm >> 1) & 0x03);
    info.max_cto     = res[3];
    info.max_dto     = info.big_endian ? (res[4] << 8 | res[5]) : (res[5] << 8 | res[4]);
    info.protocol_ver  = res[6];
    info.transport_ver = res[7];
    const int frame = static_cast<int>(transport_->max_packet());
    info.max_cto = std::clamp(info.max_cto, 8, frame);
    info.max_dto = std::clamp(info.max_dto, 8, frame);
    {
        std::lock_guard lock(mutex_);
        info_ = info;
    }
    if (info.granularity != 1)
        message(std::format("warning: address granularity {} - only byte addressing is supported", info.granularity));

    // optional: the slave's name (ASAM MC2 file name, type 1)
    try {
        const auto id = command({CC_GET_ID, 0x01});
        if (id.size() >= 8) {
            const uint32_t len = get32(&id[4]);
            if (len > 0 && len < 256) {
                std::string name;
                if (id[1] & 0x01) {   // transferred in the response (XCP 1.3: data from byte 8)
                    if (id.size() > 8) name.assign(id.begin() + 8, id.begin() + std::min<std::size_t>(id.size(), 8 + len));
                } else {
                    const auto data = upload_mta(len);
                    name.assign(data.begin(), data.end());
                }
                std::erase(name, '\0');
                info.id = name;
            }
        }
    } catch (const std::exception&) {}

    std::vector<XcpEventInfo> events;
    info.daq = info.resource & 0x04;
    if (info.daq) {
        try {
            const auto p = command({CC_GET_DAQ_PROCESSOR_INFO});
            if (p.size() >= 8) {
                info.daq_dynamic = p[1] & 0x01;
                info.timestamps  = p[1] & 0x10;
                info.max_daq     = get16(&p[2]);
                info.max_events  = get16(&p[4]);
                info.min_daq     = p[6];
                info.id_field    = (p[7] >> 6) & 0x03;
            }
            const auto r = command({CC_GET_DAQ_RESOLUTION_INFO});
            if (r.size() >= 8) {
                info.max_odt_entry = r[2];
                const uint8_t mode = r[5];
                info.ts_size  = mode & 0x07;
                info.ts_tick_s = unit_seconds(mode >> 4) * std::max<uint16_t>(1, get16(&r[6]));
                if (info.ts_size != 1 && info.ts_size != 2 && info.ts_size != 4) info.timestamps = false;
            }
            for (int e = 0; e < info.max_events && e < 256; ++e) {
                XcpEventInfo ev;
                ev.number = e;
                ev.name   = std::format("Event {}", e);
                try {
                    std::vector<uint8_t> cro{CC_GET_DAQ_EVENT_INFO, 0x00};
                    put16(cro, static_cast<uint16_t>(e));
                    const auto er = command(cro);
                    if (er.size() >= 7) {
                        ev.max_lists = er[2];
                        ev.cycle_s   = er[4] * unit_seconds(er[5]);
                        if (er[3] > 0) {
                            const auto name = upload_mta(er[3]);
                            ev.name.assign(name.begin(), name.end());
                            std::erase(ev.name, '\0');
                        }
                    }
                } catch (const XcpError& err) {
                    if (err.code == ERR_CMD_UNKNOWN) { events.push_back(ev); continue; }
                    throw;
                }
                events.push_back(std::move(ev));
            }
        } catch (const XcpError& e) {
            message(std::string("DAQ info: ") + e.what());
        }
        if (!info.daq_dynamic) message("the slave has no dynamic DAQ lists - variables are polled");
    }
    {
        std::lock_guard lock(mutex_);
        info_   = info;
        events_ = std::move(events);
    }
    daq_dirty_ = true;
    set_state(State::Connected, std::format("connected (MAX_CTO {}, MAX_DTO {}, {} endian{})", info.max_cto,
                                            info.max_dto, info.big_endian ? "big" : "little",
                                            info.daq_dynamic ? ", DAQ" : ""));
    message(std::format("connected to {} on {}{}", info.id.empty() ? target_.name : info.id, target_.interface,
                        target_.fd ? " (CAN FD)" : ""));
}

void XcpMaster::do_disconnect(bool send)
{
    if (!transport_) return;
    if (send) {
        try {
            if (daq_running_) stop_daq();
            command({CC_DISCONNECT});
        } catch (const std::exception&) {}
    }
    daq_running_ = false;
    daq_.clear();
    pid_map_.clear();
    transport_.reset();
    rate_hz_ = 0.0;
}

std::vector<uint8_t> XcpMaster::command(std::vector<uint8_t> cro)
{
    const uint8_t cmd = cro.front();
    const auto timeout = std::chrono::milliseconds(std::max(1, target_.timeout_ms));
    for (int attempt = 0; attempt < 2; ++attempt) {
        transport_->send(cro);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) break;
            auto pkt = transport_->receive(left);
            if (!pkt) break;
            const uint8_t pid = pkt->front();
            if (pid == PID_RES) { timeouts_ = 0; return std::move(*pkt); }
            if (pid == PID_ERR) {
                const uint8_t code = pkt->size() > 1 ? (*pkt)[1] : 0x31;
                if (code == ERR_CMD_SYNCH) continue;   // answer to an earlier SYNCH
                throw XcpError(cmd, code);
            }
            dispatch(*pkt);
        }
        if (cmd == CC_CONNECT || cmd == CC_DISCONNECT) break;
        // timeout: SYNCH (expects ERR_CMD_SYNCH), then once more
        try {
            transport_->send(std::vector<uint8_t>{CC_SYNCH});
            const auto end = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < end) {
                auto pkt = transport_->receive(std::chrono::duration_cast<std::chrono::microseconds>(end - std::chrono::steady_clock::now()));
                if (!pkt) break;
                if (pkt->front() == PID_ERR || pkt->front() == PID_RES) break;
                dispatch(*pkt);
            }
        } catch (const std::exception&) {}
    }
    throw XcpTimeout(cmd);
}

void XcpMaster::dispatch(const std::vector<uint8_t>& pkt)
{
    const uint8_t pid = pkt.front();
    if (pid == PID_EV) {
        const uint8_t code = pkt.size() > 1 ? pkt[1] : 0;
        static constexpr const char* NAMES[] = {"EV_RESUME_MODE", "EV_CLEAR_DAQ", "EV_STORE_DAQ", "EV_STORE_CAL",
                                                "?", "EV_CMD_PENDING", "EV_DAQ_OVERLOAD", "EV_SESSION_TERMINATED",
                                                "EV_TIME_SYNC", "EV_STIM_TIMEOUT", "EV_SLEEP", "EV_WAKE_UP"};
        message(std::format("slave event: {}", code < std::size(NAMES) ? NAMES[code] : std::format("0x{:02X}", code)));
        if (code == 0x07) {   // session terminated
            daq_running_ = false;
            set_state(State::Error, "the slave terminated the session");
        }
    } else if (pid == PID_SERV) {
        if (pkt.size() > 2 && pkt[1] == 0x01) {   // SERV_TEXT
            std::string text(pkt.begin() + 2, pkt.end());
            std::erase(text, '\0');
            message("slave: " + text);
        }
    } else if (daq_running_) {
        on_dto(pkt);
    }
}

void XcpMaster::pump(std::chrono::microseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (transport_) {
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now());
        auto pkt = transport_->receive(std::max(left, std::chrono::microseconds(0)));
        if (!pkt) return;
        if (pkt->front() == PID_RES || pkt->front() == PID_ERR) continue;   // late response
        dispatch(*pkt);
        if (std::chrono::steady_clock::now() >= deadline) return;
    }
}

// ── memory access ────────────────────────────────────────────────────────────

void XcpMaster::set_mta(uint64_t address, uint8_t ext)
{
    std::vector<uint8_t> cro{CC_SET_MTA, 0, 0, ext};
    put32(cro, static_cast<uint32_t>(address));
    command(cro);
}

std::vector<uint8_t> XcpMaster::upload_mta(std::size_t n)
{
    std::vector<uint8_t> out;
    const std::size_t chunk = static_cast<std::size_t>(info_.max_cto - 1);
    while (out.size() < n) {
        const std::size_t k = std::min(chunk, n - out.size());
        const auto res = command({CC_UPLOAD, static_cast<uint8_t>(k)});
        if (res.size() < 1 + k) throw std::runtime_error("UPLOAD: short response");
        out.insert(out.end(), res.begin() + 1, res.begin() + 1 + static_cast<std::ptrdiff_t>(k));
    }
    return out;
}

std::vector<uint8_t> XcpMaster::upload(uint64_t address, uint8_t ext, std::size_t n)
{
    if (n <= static_cast<std::size_t>(info_.max_cto - 1) && info_.max_cto >= 8) {
        std::vector<uint8_t> cro{CC_SHORT_UPLOAD, static_cast<uint8_t>(n), 0, ext};
        put32(cro, static_cast<uint32_t>(address));
        const auto res = command(cro);
        if (res.size() < 1 + n) throw std::runtime_error("SHORT_UPLOAD: short response");
        return {res.begin() + 1, res.begin() + 1 + static_cast<std::ptrdiff_t>(n)};
    }
    set_mta(address, ext);
    return upload_mta(n);
}

void XcpMaster::download(uint64_t address, uint8_t ext, const std::vector<uint8_t>& data)
{
    set_mta(address, ext);
    const std::size_t chunk = static_cast<std::size_t>(info_.max_cto - 2);
    for (std::size_t off = 0; off < data.size(); off += chunk) {
        const std::size_t k = std::min(chunk, data.size() - off);
        std::vector<uint8_t> cro{CC_DOWNLOAD, static_cast<uint8_t>(k)};
        cro.insert(cro.end(), data.begin() + static_cast<std::ptrdiff_t>(off), data.begin() + static_cast<std::ptrdiff_t>(off + k));
        command(cro);   // MTA is incremented by the slave
    }
}

void XcpMaster::do_read(const XcpVar& var)
{
    const auto bytes = upload(var.address, var.ext, var.size);
    emit(var.name, now(), xcp_decode(bytes.data(), var, info_.big_endian));
}

void XcpMaster::do_write(const XcpVar& var, double value)
{
    std::vector<uint8_t> current;
    if (var.bit_size) current = upload(var.address, var.ext, var.size);
    const auto data = xcp_encode(value, var, info_.big_endian, current);
    download(var.address, var.ext, data);
    message(std::format("{} = {} written", var.name, value));
    do_read(var);
}

void XcpMaster::poll_once()
{
    const double t = now();
    for (const auto& v : vars_) {
        if (v.event >= 0) continue;
        const auto bytes = upload(v.address, v.ext, v.size);
        emit(v.name, t, xcp_decode(bytes.data(), v, info_.big_endian));
    }
}

// ── DAQ ──────────────────────────────────────────────────────────────────────

void XcpMaster::stop_daq()
{
    daq_running_ = false;
    command({CC_START_STOP_SYNCH, 0x00});   // stop all
}

void XcpMaster::setup_daq()
{
    daq_dirty_ = false;
    if (daq_running_) {
        try { stop_daq(); } catch (const std::exception& e) { message(std::string("stop DAQ: ") + e.what()); }
    }
    daq_.clear();
    pid_map_.clear();
    list_time_.clear();

    if (!info_.daq_dynamic) {   // no DAQ: everything is polled
        for (auto& v : vars_) v.event = -1;
        return;
    }
    // one DAQ list per event channel
    std::map<int, std::vector<std::size_t>> by_event;
    for (std::size_t i = 0; i < vars_.size(); ++i)
        if (vars_[i].event >= 0) by_event[vars_[i].event].push_back(i);
    if (by_event.empty()) return;

    const std::size_t header   = static_cast<std::size_t>(info_.id_field + 1);
    const std::size_t ts       = info_.timestamps ? static_cast<std::size_t>(info_.ts_size) : 0;
    const std::size_t dto      = static_cast<std::size_t>(info_.max_dto);
    const std::size_t max_entry = info_.max_odt_entry > 0 ? static_cast<std::size_t>(info_.max_odt_entry) : 255;
    // first fit into ODTs; the first ODT also carries the timestamp. If nothing fits next to the timestamp
    // (classic CAN: 8 - PID - 4 bytes), the list runs without timestamp (host receive time instead).
    const auto pack = [&](const std::vector<std::size_t>& idx, bool with_ts) {
        std::vector<Odt> odts;
        const auto capacity = [&](std::size_t odt) { return dto - header - (odt == 0 && with_ts ? ts : 0); };
        for (std::size_t i : idx) {
            const std::size_t sz = vars_[i].size;
            std::size_t o = 0;
            while (o < odts.size() && odts[o].used + sz > capacity(o)) ++o;
            if (o == odts.size()) {
                odts.emplace_back();
                if (odts[o].used + sz > capacity(o)) { odts.emplace_back(); ++o; }   // ODT 0 too small: next one
            }
            odts[o].entries.push_back({i, odts[o].used});
            odts[o].used += sz;
        }
        return odts;
    };
    for (auto& [event, idx] : by_event) {
        std::erase_if(idx, [&](std::size_t i) {
            const std::size_t sz = vars_[i].size;
            if (sz <= max_entry && sz + header <= dto && sz > 0) return false;
            message(std::format("{}: {} bytes do not fit into one DAQ entry - polled instead", vars_[i].name, sz));
            vars_[i].event = -1;
            return true;
        });
        if (idx.empty()) continue;
        DaqList list;
        list.event     = event;
        list.timestamp = info_.timestamps;
        list.odts      = pack(idx, list.timestamp);
        if (list.timestamp && list.odts.front().entries.empty()) {
            list.timestamp = false;
            list.odts      = pack(idx, false);
        }
        daq_.push_back(std::move(list));
    }
    if (daq_.empty()) return;

    try {
        command({CC_FREE_DAQ});
        std::vector<uint8_t> alloc{CC_ALLOC_DAQ, 0};
        put16(alloc, static_cast<uint16_t>(daq_.size()));
        command(alloc);
        for (std::size_t l = 0; l < daq_.size(); ++l) {
            std::vector<uint8_t> c{CC_ALLOC_ODT, 0};
            put16(c, static_cast<uint16_t>(l));
            c.push_back(static_cast<uint8_t>(daq_[l].odts.size()));
            command(c);
        }
        for (std::size_t l = 0; l < daq_.size(); ++l)
            for (std::size_t o = 0; o < daq_[l].odts.size(); ++o) {
                std::vector<uint8_t> c{CC_ALLOC_ODT_ENTRY, 0};
                put16(c, static_cast<uint16_t>(l));
                c.push_back(static_cast<uint8_t>(o));
                c.push_back(static_cast<uint8_t>(daq_[l].odts[o].entries.size()));
                command(c);
            }
        for (std::size_t l = 0; l < daq_.size(); ++l)
            for (std::size_t o = 0; o < daq_[l].odts.size(); ++o) {
                std::vector<uint8_t> ptr{CC_SET_DAQ_PTR, 0};
                put16(ptr, static_cast<uint16_t>(l));
                ptr.push_back(static_cast<uint8_t>(o));
                ptr.push_back(0);
                command(ptr);
                for (const auto& e : daq_[l].odts[o].entries) {
                    const auto& v = vars_[e.var];
                    std::vector<uint8_t> w{CC_WRITE_DAQ, 0xFF, static_cast<uint8_t>(v.size), v.ext};
                    put32(w, static_cast<uint32_t>(v.address));
                    command(w);
                }
            }
        for (std::size_t l = 0; l < daq_.size(); ++l) {
            std::vector<uint8_t> m{CC_SET_DAQ_LIST_MODE, static_cast<uint8_t>(daq_[l].timestamp ? 0x10 : 0x00)};
            put16(m, static_cast<uint16_t>(l));
            put16(m, static_cast<uint16_t>(daq_[l].event));
            m.push_back(1);   // prescaler
            m.push_back(0);   // priority
            command(m);
        }
        for (std::size_t l = 0; l < daq_.size(); ++l) {
            std::vector<uint8_t> s{CC_START_STOP_DAQ_LIST, 0x02};   // select
            put16(s, static_cast<uint16_t>(l));
            const auto res = command(s);
            daq_[l].first_pid = res.size() > 1 ? res[1] : 0;
            for (std::size_t o = 0; o < daq_[l].odts.size(); ++o)
                pid_map_[daq_[l].first_pid + static_cast<int>(o)] = {l, o};
        }
        list_time_.assign(daq_.size(), now());
        ts_anchored_ = false;
        daq_running_ = true;   // before START: the first DTOs may arrive with the response
        command({CC_START_STOP_SYNCH, 0x01});   // start selected
        std::size_t n = 0, odts = 0;
        for (const auto& l : daq_) { odts += l.odts.size(); for (const auto& o : l.odts) n += o.entries.size(); }
        message(std::format("DAQ started: {} list(s), {} ODT(s), {} variable(s)", daq_.size(), odts, n));
    } catch (const XcpError& e) {
        daq_running_ = false;
        message(std::string("DAQ setup failed, variables are polled: ") + e.what());
        for (auto& v : vars_) v.event = -1;
        daq_.clear();
        pid_map_.clear();
    }
}

void XcpMaster::on_dto(const std::vector<uint8_t>& pkt)
{
    std::size_t list = 0, odt = 0, off = 0;
    switch (info_.id_field) {
        case 0: {
            const auto it = pid_map_.find(pkt[0]);
            if (it == pid_map_.end()) return;
            std::tie(list, odt) = it->second;
            off = 1;
            break;
        }
        case 1:
            if (pkt.size() < 2) return;
            odt = pkt[0]; list = pkt[1]; off = 2;
            break;
        case 2:
            if (pkt.size() < 3) return;
            odt = pkt[0]; list = get16(&pkt[1]); off = 3;
            break;
        default:
            if (pkt.size() < 4) return;
            odt = pkt[0]; list = get16(&pkt[2]); off = 4;
            break;
    }
    if (list >= daq_.size() || odt >= daq_[list].odts.size()) return;

    if (odt == 0) {
        double t = now();
        if (daq_[list].timestamp) {
            const std::size_t n = static_cast<std::size_t>(info_.ts_size);
            if (pkt.size() < off + n) return;
            const uint64_t raw = read_raw(&pkt[off], n, info_.big_endian);
            off += n;
            const uint64_t range = n >= 8 ? 0 : uint64_t{1} << (8 * n);
            if (!ts_anchored_) {
                ts_anchored_ = true;
                ts_acc_      = 0;
                ts_offset_   = t;
            } else {
                int64_t d = static_cast<int64_t>((raw - ts_last_raw_) & (range - 1));
                if (static_cast<uint64_t>(d) > range / 2) d -= static_cast<int64_t>(range);   // slightly older sample
                ts_acc_ += d;
            }
            ts_last_raw_ = raw;
            t = ts_offset_ + static_cast<double>(ts_acc_) * info_.ts_tick_s;
        }
        list_time_[list] = t;
    }
    const double t = list_time_[list];
    for (const auto& e : daq_[list].odts[odt].entries) {
        const auto& v = vars_[e.var];
        if (pkt.size() < off + e.offset + v.size) return;
        emit(v.name, t, xcp_decode(&pkt[off + e.offset], v, info_.big_endian));
    }
}

std::ostream& operator<<(std::ostream& os, const XcpVar& v)
{
    os << "XcpVar(" << v.name << " @" << int(v.ext) << ":0x" << std::hex << v.address << std::dec << ", "
       << to_string(v.type) << ", " << v.size << " B";
    if (v.bit_size) os << ", bits " << int(v.bit_offset) << "+" << int(v.bit_size);
    return os << (v.event < 0 ? ", polled)" : ", event " + std::to_string(v.event) + ")");
}

std::ostream& operator<<(std::ostream& os, const XcpSlaveInfo& i)
{
    return os << "XcpSlaveInfo(MAX_CTO " << i.max_cto << ", MAX_DTO " << i.max_dto << ", "
              << (i.big_endian ? "big" : "little") << " endian, DAQ " << (i.daq_dynamic ? "dynamic" : i.daq ? "static" : "no")
              << ", " << i.max_events << " events, timestamps " << (i.timestamps ? std::to_string(i.ts_size) + " B" : "no") << ")";
}

std::ostream& operator<<(std::ostream& os, const XcpMaster& m)
{
    return os << "XcpMaster(" << m.target_.name << " on " << m.target_.interface << ", " << to_string(m.state()) << ")";
}
