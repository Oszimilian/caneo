#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <sstream>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Serializable description of an action. Everything needed to (re)create the
// action and to write it back to caneo.yaml.
struct ActionSpec {
    std::string type;        // "single" | "periodic" | "sin" | "lua"
    std::string interface;   // filled from the owning interface entry
    std::string name;        // optional display name
    std::string msg;         // DBC message name (single/periodic/sin)
    std::vector<std::pair<std::string, double>> signals; // preset signal values
    int         period_ms = 100;   // send interval (periodic/sin/lua)
    std::string signal;            // sin: modulated signal
    double      amplitude     = 1.0;
    double      offset        = 0.0;
    int         sin_period_ms = 1000;
    std::string script;            // lua: path to the script file
    bool        paused = true;     // state on load / state when saved

    bool operator==(const ActionSpec&) const = default;
};

// Watches one signal of one message: every received frame of that message
// runs check(value, t) of a Lua script; its return values are the outputs.
struct GuardSpec {
    std::string name;        // display/log name; defaults to the script name
    std::string interface;
    std::string msg;         // DBC message name
    std::string signal;      // input signal
    std::string script;      // path to the Lua script
    bool        enabled = true;

    bool operator==(const GuardSpec&) const = default;
};

// One block of a flow (node editor). Which fields are used depends on type:
//   can_in     interface, msg, signals  -> one output port per signal
//   can_out    interface, msg, signals  -> one input port per signal, sent every period_ms
//   lua        code, signals (output names) -> 1 input, 1+ outputs: step(u, t); a returned table is
//              mapped to the outputs by name (or position). send()/send_raw() send CAN frames,
//              `interface` is the default interface for them.
//   (can_out)  sig_values: constant value per signal while its input is unconnected;
//              period_ms 0 = send once when the flow starts
//   generator  wave, amplitude, offset, frequency, phase, duty -> 1 output
//   scope      name                     -> 1 input, plotted/logged
//   const      offset (= the value)     -> 1 output, constant number
//   add        2 inputs -> a + b
//   diff       1 input  -> difference to the previous step (derivative if `derivative`)
//   sin, cos   1 input  -> sin / cos of the input (radians, or degrees if `degrees`)
//   mul        2 inputs -> a * b
//   gain       1 input  -> input * `gain`
//   integrator 1 input  -> time integral of the input (1/s), starts at `offset`
//   compare    2 inputs -> 1 if `op` (< > >= <= ==) holds for a, b else 0
//   movavg     1 input  -> mean of the last `window` steps
//   csv        N inputs (named by `signals`) -> table that can be exported to `path` (Debug flows)
//   slider     -        -> 1 output, set with a slider in the editor (min, max, value); live changes need no Apply
//   button     -        -> 1 output, 1 for one step when the button is pressed, else 0
//   rs         2 inputs S, R -> Q, !Q: RS flip-flop (op "R" = reset dominant, "S" = set dominant)
//   switch     3 inputs a, b, ctrl -> b if ctrl > 0.5 else a
//   fft        1 input  -> dominant frequency [Hz] and its amplitude over the last `window` steps
struct FlowNodeSpec {
    int         id = 0;
    std::string type;
    std::string name;
    float       x = 0.0f, y = 0.0f;   // position in the editor

    std::string              interface;
    std::string              msg;
    std::vector<std::string> signals;
    int                      period_ms = 100;   // can_out

    std::string code;                           // lua

    std::string wave      = "sin";   // sin | pulse | ramp | sawtooth | triangle | noise | const
    double      amplitude = 1.0;
    double      offset    = 0.0;
    double      frequency = 1.0;     // Hz
    double      phase     = 0.0;     // degrees
    double      duty      = 0.5;     // pulse: high fraction

    std::vector<double> sig_values;     // can_out: constants for unconnected inputs
    std::string path;                   // csv: output file
    std::string op         = "<";    // compare
    int         window     = 16;     // movavg: samples, fft: samples (power of two)
    bool        derivative = true;   // diff: divide by the step time
    std::vector<std::string> inputs;     // lua: input names (empty = one input u); step(in1, in2, ..., t) for more than one
    bool        degrees    = false;  // sin / cos: input in degrees instead of radians
    double      gain       = 1.0;    // gain: factor; integrator (1/s): `offset` = initial value
    double      min = 0.0, max = 100.0, value = 0.0;   // slider: range and current value

    bool operator==(const FlowNodeSpec&) const = default;
};

struct FlowLinkSpec {
    int from_node = 0, from_port = 0;
    int to_node   = 0, to_port   = 0;

    bool operator==(const FlowLinkSpec&) const = default;
};

// A block diagram: CAN signals in, Lua/generator blocks, CAN messages / scopes
// out. Evaluated every step_ms.
struct FlowSpec {
    std::string               name;
    bool                      enabled = true;
    int                       step_ms = 10;
    std::vector<FlowNodeSpec> nodes;
    std::vector<FlowLinkSpec> links;

    bool operator==(const FlowSpec&) const = default;
};

struct GraphSignalConfig {
    std::string           interface;   // empty = search all interfaces
    std::string           msg;         // empty for model outputs
    std::string           signal;
    int                   y_axis = 0;  // 0=Y1, 1=Y2, 2=Y3
    std::optional<double> y_min;       // overrides the DBC range
    std::optional<double> y_max;
};

struct GraphConfig {
    std::string                    name;
    double                         window_s = 30.0; // visible time window
    bool                           auto_y   = true;
    std::vector<GraphSignalConfig> signals;
};

struct GuiConfig {
    float       scale = 1.0f;
    std::string theme = "light";   // "light" | "dark"
    int         autosave_s = 0;    // save the config every N seconds if changed (0 = off)

    bool operator==(const GuiConfig&) const = default;
};

// What goes into the MCAP log. Every logged message costs ~16 bytes of
// (uncompressed) message index on top of its compressed data, so the number
// of channels per frame dominates the file size.
struct LogOptions {
    // "mcap": decoded signals + guards, for PlotJuggler/Foxglove
    // "blf":  raw frames only (Vector BLF), small, for CANalyzer/asammdf
    // "both": both files side by side (same name, different extension)
    std::string format   = "mcap";
    bool raw             = true;  // <iface>/<msg>/raw: original bytes, needed for --playback
    bool update_ratio    = true;  // <iface>/<msg>/update_ratio: time since the previous frame
    int  min_interval_ms = 0;     // log each CAN ID at most every N ms (0 = every frame)
    int  flush_s         = 10;    // write buffered data to disk every N s (0 = only on stop)

    bool mcap() const { return format != "blf"; }
    bool blf()  const { return format == "blf" || format == "both"; }

    bool operator==(const LogOptions&) const = default;
};

// One tab of the Debug view: plots, each a list of series labels "iface/msg/signal".
struct DebugTabConfig {
    std::string                           name;
    std::vector<std::vector<std::string>> plots;
    bool                                  xy = false;   // XY plot: plots[0] = {x signal, y signal}

    bool operator==(const DebugTabConfig&) const = default;
};

// A Debug session (log file, DBCs, plot layout); the last ones are kept in
// config `debug_history:` and can be reloaded with one click.
struct DebugSession {
    std::string                        name;    // file name
    std::string                        log;     // path of the MCAP / BLF
    std::string                        saved;   // when it was stored, "YYYY-MM-DD HH:MM"
    std::map<std::string, std::string> dbcs;    // interface -> DBC
    std::vector<DebugTabConfig>        tabs;

    // same session ignoring the timestamp
    bool same_content(const DebugSession& o) const {
        return log == o.log && dbcs == o.dbcs && tabs == o.tabs;
    }
};

struct InterfaceConfig {
    std::string             name;
    std::string             dbc;           // empty if not set
    uint32_t                baudrate = 0;  // 0 if not set
    bool                    fd = false;            // CAN FD (bitrate + dbitrate, vcan: mtu 72)
    uint32_t                data_baudrate = 0;     // CAN FD data phase bit/s (0 = not set)
    bool                    virtual_can = false;   // create as vcan device (in addition to Config::virtual_can)
    bool                    live = true;           // frames go to the live view (trace windows)
    bool                    log  = true;           // frames are logged
    std::vector<ActionSpec> actions;
};

// One block of the input pipeline (GUI "Inputs" window): where CAN data comes from and where it goes.
//   hw_socketcan       device (USB id, may be empty), channels (interface names) -> one output port per channel
//   virtual_socketcan  channels (vcan interface names)                           -> one output port per channel
//   baudrate           baudrate or virtual_can, fd + data_baudrate (CAN FD)      1 in -> 1 out
//   xcp                xcp_* (CAN ids, address extension, timeout), name        1 in (XCP master on that channel)
//   decode             dbc                                                       1 in -> 1 out
//   live               -                                                         inputs -> live view
//   log                format (mcap | blf)                                       inputs -> log file
struct PipelineNodeSpec {
    int         id = 0;
    std::string type;
    std::string name;
    float       x = 0.0f, y = 0.0f;

    std::string              device;     // hw_socketcan: "usb:<vid>:<pid>:<serial>" / "dev:<name>"; empty = by interface name
    std::vector<std::string> channels;
    uint32_t                 baudrate    = 500'000;
    bool                     virtual_can = false;   // baudrate: no real bus, create a vcan device
    bool                     fd = false;            // baudrate: CAN FD
    uint32_t                 data_baudrate = 2'000'000;   // baudrate: CAN FD data phase
    std::string              dbc;
    // xcp block
    uint32_t                 xcp_tx_id      = 0x7F0;   // master -> slave (CRO / commands)
    uint32_t                 xcp_rx_id      = 0x7F1;   // slave -> master (responses, DAQ)
    int                      xcp_addr_ext   = 0;       // address extension of all memory accesses
    int                      xcp_timeout_ms = 100;     // XCP T1
    bool                     xcp_pad        = false;   // pad frames to 8 / the next CAN FD length (MAX_DLC_REQUIRED)
    std::string              format = "mcap";

    bool operator==(const PipelineNodeSpec&) const = default;
};

struct PipelineLinkSpec {
    int from_node = 0, from_port = 0;
    int to_node   = 0, to_port   = 0;

    bool operator==(const PipelineLinkSpec&) const = default;
};

struct PipelineSpec {
    std::vector<PipelineNodeSpec> nodes;
    std::vector<PipelineLinkSpec> links;

    bool operator==(const PipelineSpec&) const = default;
};

// One XCP slave (derived from an `xcp` pipeline block): CAN transport + addressing.
struct XcpTargetConfig {
    std::string name;            // block name, default "XCP <interface>"
    std::string interface;
    uint32_t    tx_id      = 0x7F0;
    uint32_t    rx_id      = 0x7F1;
    bool        fd         = false;   // the interface runs CAN FD -> XCP on CAN FD
    int         addr_ext   = 0;
    int         timeout_ms = 100;
    bool        pad        = false;

    bool operator==(const XcpTargetConfig&) const = default;
};

// One measured variable of the XCP mode: event -1 = polled (SHORT_UPLOAD), otherwise the DAQ event channel.
struct XcpMeasureConfig {
    std::string name;
    int         event = -1;

    bool operator==(const XcpMeasureConfig&) const = default;
};

// State of the XCP mode (config `xcp:`): ELF / A2L, measured variables, plot layout.
struct XcpConfig {
    std::string                           target;    // XcpTargetConfig::name
    std::string                           elf;
    std::string                           a2l;       // A2L (ASAP2) description; variables from here, addresses from the ELF
    int                                   poll_ms = 50;
    std::vector<XcpMeasureConfig>         measure;
    std::vector<std::vector<std::string>> plots;      // variable names per plot

    bool operator==(const XcpConfig&) const = default;
};

struct Config {
    bool                         virtual_can = false;
    std::string                  log_file_path;     // as written, may start with ~/
    std::string                  log_viewer;        // command to open a log, {file} = path
    LogOptions                   logging;
    GuiConfig                    gui;
    std::vector<InterfaceConfig> interfaces;     // derived from `pipeline` when that exists (see Pipeline.hpp)
    PipelineSpec                 pipeline;       // config `pipeline:` - the graphical channel setup of the GUI
    std::vector<GraphConfig>     graphs;
    std::vector<GuardSpec>       guards;
    std::vector<FlowSpec>        flows;
    std::vector<DebugSession>    debug_history;   // newest first
    std::map<int, std::string>   blf_channels;    // Debug view: BLF channel number (1-based) -> interface name
    std::vector<std::string>     dbc_history;     // DBC files opened / saved in the DBC mode, newest first
    std::vector<FlowSpec>        debug_flows;     // offline flows of the Debug view (run on loaded logs)
    std::vector<XcpTargetConfig> xcp_targets;     // derived from the `xcp` pipeline blocks (not saved separately)
    XcpConfig                    xcp;             // config `xcp:` - XCP mode

    // log_file_path with ~ expanded; nullopt if not set.
    std::optional<std::filesystem::path> log_dir() const;

    // True if the parts that need a restart (sockets, decoders) differ.
    bool same_channels(const Config& other) const;
};

// Loads caneo.yaml. Also understands the legacy layout, where graphs
// (graphs_preset:) and per-interface actions (actions: <file>) live in
// separate files; their content is merged into the returned Config.
Config                load_config(const std::string& path);
std::optional<Config> try_load_default_config();

// Writes everything (channels, gui, actions, graphs) into one YAML file.
void save_config(const Config& config, const std::string& path);

// The YAML text save_config() writes (used to detect changes for autosave).
std::string config_to_yaml(const Config& config);

std::filesystem::path expand_path(const std::string& raw);

std::ostream& operator<<(std::ostream& os, const ActionSpec& spec);
std::ostream& operator<<(std::ostream& os, const Config& config);
std::ostream& operator<<(std::ostream& os, const GuardSpec& spec);
std::ostream& operator<<(std::ostream& os, const FlowSpec& spec);

template <>
struct std::formatter<GuardSpec> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const GuardSpec& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};

template <>
struct std::formatter<ActionSpec> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const ActionSpec& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};

template <>
struct std::formatter<Config> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const Config& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};

template <>
struct std::formatter<FlowSpec> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const FlowSpec& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
