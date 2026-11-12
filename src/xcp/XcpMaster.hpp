#pragma once

#include "config/Config.hpp"
#include "xcp/ElfReader.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <variant>
#include <vector>

class XcpCanTransport;

// One variable as the master reads / writes it.
struct XcpVar {
    std::string name;
    uint64_t    address    = 0;
    uint8_t     ext        = 0;
    uint32_t    size       = 0;   // bytes of the storage (bitfield: the storage unit)
    XcpType     type       = XcpType::U8;
    uint8_t     bit_offset = 0, bit_size = 0;
    int         event      = -1;  // -1 = polled with SHORT_UPLOAD, otherwise DAQ event channel
    XcpConversion conv;           // raw <-> physical (A2L)

    static XcpVar from(const ElfVariable& v, uint8_t ext, int event = -1);
    bool operator==(const XcpVar&) const = default;
};

struct XcpEventInfo {
    int         number = 0;
    std::string name;
    double      cycle_s = 0.0;   // 0 = sporadic / unknown
    int         max_lists = 1;
};

// What CONNECT / GET_DAQ_* told us about the slave.
struct XcpSlaveInfo {
    uint8_t     resource      = 0;   // bit 0 CAL/PAG, 2 DAQ, 3 STIM, 4 PGM
    bool        big_endian    = false;
    int         granularity   = 1;   // bytes per address
    int         max_cto       = 8;
    int         max_dto       = 8;
    int         protocol_ver  = 0, transport_ver = 0;
    std::string id;                  // GET_ID (ASAM MC2 name), if the slave has one
    bool        daq           = false;
    bool        daq_dynamic   = false;
    bool        timestamps    = false;
    int         ts_size       = 0;
    double      ts_tick_s     = 0.0;
    int         max_daq       = 0, min_daq = 0, max_events = 0;
    int         id_field      = 0;   // 0 abs ODT, 1 rel ODT + DAQ byte, 2 + DAQ word, 3 + fill + DAQ word
    int         max_odt_entry = 0;
};

// XCP master (ASAM MCD-1 XCP 1.x) on CAN / CAN FD. All bus traffic runs on an own worker thread; the public
// functions only queue requests and are safe to call from the GUI thread.
//   - polling: SHORT_UPLOAD of every polled variable every poll_ms
//   - DAQ:     one dynamic DAQ list per event channel; the slave sends the variables in every cycle of the event
//   - write:   SET_MTA + DOWNLOAD (bitfields: read-modify-write)
class XcpMaster {
public:
    enum class State { Disconnected, Connecting, Connected, Error };
    // name, time in s since connect, physical value. Called on the worker thread.
    using SampleFn = std::function<void(const std::string& name, double t, double value)>;

    explicit XcpMaster(XcpTargetConfig target);
    ~XcpMaster();
    XcpMaster(const XcpMaster&) = delete;
    XcpMaster& operator=(const XcpMaster&) = delete;

    void set_sample_fn(SampleFn fn);   // before connect(); read by the worker without a lock

    void connect();
    void disconnect();
    // Replaces the measured variables (polled and DAQ); a running DAQ is set up again.
    void set_measurements(std::vector<XcpVar> vars, int poll_ms);
    // Writes one value; afterwards it is read back (-> sample fn).
    void write(XcpVar var, double value);
    // Reads one value once (-> sample fn).
    void read(XcpVar var);

    State                     state() const;
    std::string               status() const;        // last state message
    XcpSlaveInfo              info() const;
    std::vector<XcpEventInfo> events() const;
    bool                      daq_running() const { return daq_running_; }
    double                    rate_hz() const { return rate_hz_; }   // received samples per second
    const XcpTargetConfig&    target() const { return target_; }
    // Log lines (commands that failed, events of the slave, ...) since the last call.
    std::vector<std::string>  take_messages();

    friend std::ostream& operator<<(std::ostream& os, const XcpMaster& m);

private:
    struct Connect {};
    struct Disconnect {};
    struct Measure { std::vector<XcpVar> vars; int poll_ms; };
    struct Write { XcpVar var; double value; };
    struct Read { XcpVar var; };
    using Request = std::variant<Connect, Disconnect, Measure, Write, Read>;

    struct OdtEntry { std::size_t var; std::size_t offset; };
    struct Odt { std::vector<OdtEntry> entries; std::size_t used = 0; };
    struct DaqList {
        int              event = 0;
        std::vector<Odt> odts;
        int              first_pid = 0;
        bool             timestamp = false;
    };

    void post(Request r);
    void run(std::stop_token st);
    void handle(Request& r);
    void do_connect();
    void do_disconnect(bool send);
    void do_write(const XcpVar& var, double value);
    void do_read(const XcpVar& var);
    void setup_daq();
    void stop_daq();
    void poll_once();
    void pump(std::chrono::microseconds timeout);   // receives DAQ / events until timeout

    // Sends a command, returns the positive response. Throws XcpError / std::runtime_error.
    std::vector<uint8_t> command(std::vector<uint8_t> cro);
    void                 dispatch(const std::vector<uint8_t>& pkt);   // DAQ / EV / SERV packets
    void                 on_dto(const std::vector<uint8_t>& pkt);
    std::vector<uint8_t> upload(uint64_t address, uint8_t ext, std::size_t n);
    std::vector<uint8_t> upload_mta(std::size_t n);   // UPLOAD from the current MTA
    void                 download(uint64_t address, uint8_t ext, const std::vector<uint8_t>& data);
    void                 set_mta(uint64_t address, uint8_t ext);

    double now() const;
    void   emit(const std::string& name, double t, double v);
    void   set_state(State s, std::string msg);
    void   message(std::string m);

    // byte order of the slave
    void     put16(std::vector<uint8_t>& b, uint16_t v) const;
    void     put32(std::vector<uint8_t>& b, uint32_t v) const;
    uint16_t get16(const uint8_t* p) const;
    uint32_t get32(const uint8_t* p) const;

    XcpTargetConfig                  target_;
    SampleFn                         sample_fn_;
    std::unique_ptr<XcpCanTransport> transport_;

    mutable std::mutex      mutex_;          // state_, status_, info_, events_, messages_, queue_
    std::condition_variable cv_;
    std::deque<Request>     queue_;
    State                   state_ = State::Disconnected;
    std::string             status_ = "not connected";
    XcpSlaveInfo            info_;
    std::vector<XcpEventInfo> events_;
    std::vector<std::string>  messages_;

    // worker thread only
    std::vector<XcpVar>   vars_;
    int                   poll_ms_ = 50;
    std::vector<DaqList>  daq_;
    std::map<int, std::pair<std::size_t, std::size_t>> pid_map_;   // abs PID -> (list, odt)
    bool                  daq_dirty_ = false;
    std::chrono::steady_clock::time_point t0_, next_poll_;
    int                   timeouts_ = 0;
    // DAQ timestamps -> seconds: unwrapped tick counter, anchored at the first DTO
    bool     ts_anchored_ = false;
    uint64_t ts_last_raw_ = 0;
    int64_t  ts_acc_      = 0;
    double   ts_offset_   = 0.0;
    std::vector<double> list_time_;   // time of the current sample per DAQ list
    std::size_t samples_ = 0;
    std::chrono::steady_clock::time_point rate_t_;

    std::atomic<bool>   daq_running_{false};
    std::atomic<double> rate_hz_{0.0};
    std::jthread        thread_;   // last: started after all members exist
};

const char* to_string(XcpMaster::State s);
std::ostream& operator<<(std::ostream& os, const XcpVar& v);
std::ostream& operator<<(std::ostream& os, const XcpSlaveInfo& i);
std::string xcp_error_name(uint8_t code);

// Physical value of a variable from its bytes (slave byte order), `conv` applied.
double xcp_decode(const uint8_t* p, const XcpVar& v, bool big_endian);
// Raw bytes for a physical value (`conv` inverted, then rounded / range-checked); for bitfields `current` (the storage unit as read) is modified.
std::vector<uint8_t> xcp_encode(double value, const XcpVar& v, bool big_endian, const std::vector<uint8_t>& current = {});

template <>
struct std::formatter<XcpVar> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const XcpVar& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};

template <>
struct std::formatter<XcpSlaveInfo> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const XcpSlaveInfo& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};

template <>
struct std::formatter<XcpMaster> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const XcpMaster& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
