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
//   compare    2 inputs -> 1 if `op` (< > >= <= ==) holds for a, b else 0
//   movavg     1 input  -> mean of the last `window` steps
//   csv        N inputs (named by `signals`) -> table that can be exported to `path` (Debug flows)
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
    std::vector<ActionSpec> actions;
};

struct Config {
    bool                         virtual_can = false;
    std::string                  log_file_path;     // as written, may start with ~/
    std::string                  log_viewer;        // command to open a log, {file} = path
    LogOptions                   logging;
    GuiConfig                    gui;
    std::vector<InterfaceConfig> interfaces;
    std::vector<GraphConfig>     graphs;
    std::vector<GuardSpec>       guards;
    std::vector<FlowSpec>        flows;
    std::vector<DebugSession>    debug_history;   // newest first
    std::vector<std::string>     dbc_history;     // DBC files opened / saved in the DBC mode, newest first
    std::vector<FlowSpec>        debug_flows;     // offline flows of the Debug view (run on loaded logs)

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
