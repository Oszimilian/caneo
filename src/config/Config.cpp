#include "Config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

std::filesystem::path expand_path(const std::string& raw) {
    if (raw == ".")
        return std::filesystem::current_path();
    if (raw.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        if (home)
            return std::filesystem::path(home) / raw.substr(2);
    }
    return raw;
}

std::optional<std::filesystem::path> Config::log_dir() const {
    if (log_file_path.empty())
        return std::nullopt;
    return expand_path(log_file_path);
}

bool Config::same_channels(const Config& other) const {
    if (virtual_can != other.virtual_can || interfaces.size() != other.interfaces.size())
        return false;
    for (std::size_t i = 0; i < interfaces.size(); ++i) {
        const auto& a = interfaces[i];
        const auto& b = other.interfaces[i];
        if (a.name != b.name || a.dbc != b.dbc || a.baudrate != b.baudrate)
            return false;
    }
    return true;
}

// ── parsing ─────────────────────────────────────────────────────────────────

template <typename T>
static T get_or(const YAML::Node& node, const char* key, T fallback) {
    return node[key] ? node[key].as<T>() : fallback;
}

static ActionSpec parse_action(const YAML::Node& n, const std::string& iface) {
    ActionSpec a;
    a.interface = iface;
    a.type      = get_or<std::string>(n, "type", "");
    a.name      = get_or<std::string>(n, "name", "");
    a.msg       = get_or<std::string>(n, "msg", "");
    a.signal    = get_or<std::string>(n, "signal", "");
    a.script    = get_or<std::string>(n, "script", "");
    a.amplitude = get_or<double>(n, "amplitude", a.amplitude);
    a.offset    = get_or<double>(n, "offset", a.offset);
    a.sin_period_ms = get_or<int>(n, "sin_period_ms", a.sin_period_ms);
    a.paused    = get_or<bool>(n, "paused", a.paused);
    // Sin actions historically used interval_ms for the send interval.
    a.period_ms = get_or<int>(n, "period_ms", get_or<int>(n, "interval_ms", a.period_ms));
    if (a.type == "sin" && !n["period_ms"] && !n["interval_ms"])
        a.period_ms = 20;
    if (const auto& sigs = n["signals"]; sigs && sigs.IsMap())
        for (const auto& kv : sigs)
            a.signals.emplace_back(kv.first.as<std::string>(), kv.second.as<double>());
    return a;
}

static std::vector<ActionSpec> parse_actions(const YAML::Node& node, const std::string& iface) {
    std::vector<ActionSpec> out;
    YAML::Node list = node;
    if (node.IsScalar()) {
        // Legacy: actions: config/foo_actions.yaml
        const std::string file = node.as<std::string>();
        try {
            list = YAML::LoadFile(expand_path(file).string());
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to load actions file '" + file + "': " + e.what());
        }
    }
    if (!list.IsSequence())
        return out;
    for (const auto& entry : list)
        out.push_back(parse_action(entry, iface));
    return out;
}

static std::vector<GraphConfig> parse_graphs(const YAML::Node& list) {
    std::vector<GraphConfig> out;
    if (!list || !list.IsSequence())
        return out;
    for (const auto& g : list) {
        GraphConfig gc;
        gc.name     = get_or<std::string>(g, "name", "");
        gc.window_s = get_or<double>(g, "window_s", gc.window_s);
        gc.auto_y   = get_or<bool>(g, "auto_y", gc.auto_y);
        if (const auto& sigs = g["signals"]; sigs && sigs.IsSequence()) {
            for (const auto& s : sigs) {
                GraphSignalConfig sc;
                sc.interface = get_or<std::string>(s, "interface", "");
                sc.msg       = get_or<std::string>(s, "msg", "");
                sc.signal    = get_or<std::string>(s, "signal", "");
                sc.y_axis    = get_or<int>(s, "y_axis", 0);
                if (s["y_min"]) sc.y_min = s["y_min"].as<double>();
                if (s["y_max"]) sc.y_max = s["y_max"].as<double>();
                if (!sc.signal.empty())
                    gc.signals.push_back(std::move(sc));
            }
        }
        out.push_back(std::move(gc));
    }
    return out;
}

static std::vector<GuardSpec> parse_guards(const YAML::Node& list) {
    std::vector<GuardSpec> out;
    if (!list || !list.IsSequence())
        return out;
    for (const auto& n : list) {
        GuardSpec g;
        g.name      = get_or<std::string>(n, "name", "");
        g.interface = get_or<std::string>(n, "interface", "");
        g.msg       = get_or<std::string>(n, "msg", "");
        g.signal    = get_or<std::string>(n, "signal", "");
        g.script    = get_or<std::string>(n, "script", "");
        g.enabled   = get_or<bool>(n, "enabled", true);
        out.push_back(std::move(g));
    }
    return out;
}

static std::vector<DebugSession> parse_debug_history(const YAML::Node& list) {
    std::vector<DebugSession> out;
    if (!list || !list.IsSequence())
        return out;
    for (const auto& n : list) {
        DebugSession d;
        d.name  = get_or<std::string>(n, "name", "");
        d.log   = get_or<std::string>(n, "log", "");
        d.saved = get_or<std::string>(n, "saved", "");
        if (d.log.empty())
            continue;
        if (n["dbcs"] && n["dbcs"].IsMap())
            for (const auto& kv : n["dbcs"])
                d.dbcs[kv.first.as<std::string>()] = kv.second.as<std::string>();
        if (n["tabs"] && n["tabs"].IsSequence()) {
            for (const auto& t : n["tabs"]) {
                DebugTabConfig tab;
                tab.name = get_or<std::string>(t, "name", "Plot");
                if (t["plots"] && t["plots"].IsSequence())
                    for (const auto& p : t["plots"])
                        tab.plots.push_back(p.as<std::vector<std::string>>());
                d.tabs.push_back(std::move(tab));
            }
        }
        out.push_back(std::move(d));
    }
    return out;
}

static std::vector<FlowSpec> parse_flows(const YAML::Node& list) {
    std::vector<FlowSpec> out;
    if (!list || !list.IsSequence())
        return out;
    for (const auto& n : list) {
        FlowSpec f;
        f.name    = get_or<std::string>(n, "name", "");
        f.enabled = get_or<bool>(n, "enabled", true);
        f.step_ms = get_or<int>(n, "step_ms", 10);
        if (const auto& nodes = n["nodes"]; nodes && nodes.IsSequence()) {
            for (const auto& nn : nodes) {
                FlowNodeSpec s;
                s.id        = get_or<int>(nn, "id", 0);
                s.type      = get_or<std::string>(nn, "type", "");
                s.name      = get_or<std::string>(nn, "name", "");
                s.x         = get_or<float>(nn, "x", 0.0f);
                s.y         = get_or<float>(nn, "y", 0.0f);
                s.interface = get_or<std::string>(nn, "interface", "");
                s.msg       = get_or<std::string>(nn, "msg", "");
                s.period_ms = get_or<int>(nn, "period_ms", 100);
                s.code      = get_or<std::string>(nn, "code", "");
                s.wave      = get_or<std::string>(nn, "wave", "sin");
                s.amplitude = get_or<double>(nn, "amplitude", 1.0);
                s.offset    = get_or<double>(nn, "offset", 0.0);
                s.frequency = get_or<double>(nn, "frequency", 1.0);
                s.phase     = get_or<double>(nn, "phase", 0.0);
                s.duty      = get_or<double>(nn, "duty", 0.5);
                s.path       = get_or<std::string>(nn, "path", "");
                if (const auto& sv = nn["sig_values"]; sv && sv.IsSequence())
                    for (const auto& v : sv) s.sig_values.push_back(v.as<double>());
                s.op         = get_or<std::string>(nn, "op", "<");
                s.window     = get_or<int>(nn, "window", 16);
                s.derivative = get_or<bool>(nn, "derivative", true);
                if (const auto& sigs = nn["signals"]; sigs && sigs.IsSequence())
                    for (const auto& sg : sigs)
                        s.signals.push_back(sg.as<std::string>());
                f.nodes.push_back(std::move(s));
            }
        }
        if (const auto& links = n["links"]; links && links.IsSequence()) {
            for (const auto& l : links) {
                if (!l.IsSequence() || l.size() != 4) continue;   // [from_node, from_port, to_node, to_port]
                f.links.push_back({ l[0].as<int>(), l[1].as<int>(), l[2].as<int>(), l[3].as<int>() });
            }
        }
        out.push_back(std::move(f));
    }
    return out;
}

static Config parse_config(const YAML::Node& root) {
    Config result;

    result.virtual_can   = get_or<bool>(root, "virtual", false);
    result.log_file_path = get_or<std::string>(root, "log_file_path", "");
    result.log_viewer    = get_or<std::string>(root, "log_viewer", "");

    if (const auto& gui = root["gui"]) {
        result.gui.scale      = get_or<float>(gui, "scale", result.gui.scale);
        result.gui.theme      = get_or<std::string>(gui, "theme", result.gui.theme);
        result.gui.autosave_s = get_or<int>(gui, "autosave_s", result.gui.autosave_s);
    }

    if (const auto& log = root["logging"]) {
        auto& o = result.logging;
        o.format          = get_or<std::string>(log, "format", o.format);
        if (o.format != "mcap" && o.format != "blf" && o.format != "both")
            throw std::runtime_error("logging.format must be mcap, blf or both");
        o.raw             = get_or<bool>(log, "raw", o.raw);
        o.update_ratio    = get_or<bool>(log, "update_ratio", o.update_ratio);
        o.min_interval_ms = get_or<int>(log, "min_interval_ms", o.min_interval_ms);
        o.flush_s         = get_or<int>(log, "flush_s", o.flush_s);
    }

    if (root["graphs"]) {
        result.graphs = parse_graphs(root["graphs"]);
    } else if (root["graphs_preset"]) {
        // Legacy: graphs_preset: config/foo_graphs.yaml
        const std::string file = root["graphs_preset"].as<std::string>();
        try {
            result.graphs = parse_graphs(YAML::LoadFile(expand_path(file).string()));
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to load graphs preset '" + file + "': " + e.what());
        }
    }

    result.guards = parse_guards(root["guards"]);
    result.flows  = parse_flows(root["flows"]);
    result.debug_history = parse_debug_history(root["debug_history"]);
    result.debug_flows   = parse_flows(root["debug_flows"]);
    if (root["dbc_history"] && root["dbc_history"].IsSequence())
        for (const auto& n : root["dbc_history"])
            result.dbc_history.push_back(n.as<std::string>());

    const auto& interfaces = root["interfaces"];
    if (!interfaces)
        throw std::runtime_error("Config: missing 'interfaces' key");

    for (const auto& entry : interfaces) {
        InterfaceConfig cfg;
        cfg.name = entry.first.as<std::string>();
        if (entry.second) {
            if (entry.second.IsScalar()) {
                // Old format: vcan0: dbc/vcan0.dbc
                cfg.dbc = entry.second.as<std::string>();
            } else if (entry.second.IsMap()) {
                // New format: vcan0: { dbc: ..., baudrate: ..., actions: [...] }
                cfg.dbc      = get_or<std::string>(entry.second, "dbc", "");
                cfg.baudrate = get_or<uint32_t>(entry.second, "baudrate", 0);
                if (entry.second["actions"])
                    cfg.actions = parse_actions(entry.second["actions"], cfg.name);
            }
        }
        result.interfaces.push_back(std::move(cfg));
    }
    return result;
}

Config load_config(const std::string& path) {
    return parse_config(YAML::LoadFile(path));
}

std::optional<Config> try_load_default_config() {
    const std::filesystem::path p = "caneo.yaml";
    if (!std::filesystem::exists(p))
        return std::nullopt;
    return load_config(p.string());
}

// ── saving ──────────────────────────────────────────────────────────────────

static void emit_action(YAML::Emitter& out, const ActionSpec& a) {
    out << YAML::BeginMap;
    out << YAML::Key << "type" << YAML::Value << a.type;
    if (!a.name.empty())
        out << YAML::Key << "name" << YAML::Value << a.name;
    if (a.type == "lua") {
        out << YAML::Key << "script" << YAML::Value << a.script;
    } else {
        out << YAML::Key << "msg" << YAML::Value << a.msg;
    }
    out << YAML::Key << "period_ms" << YAML::Value << a.period_ms;
    if (a.type == "sin") {
        out << YAML::Key << "signal"        << YAML::Value << a.signal;
        out << YAML::Key << "amplitude"     << YAML::Value << a.amplitude;
        out << YAML::Key << "offset"        << YAML::Value << a.offset;
        out << YAML::Key << "sin_period_ms" << YAML::Value << a.sin_period_ms;
    }
    if (!a.signals.empty() && a.type != "lua") {
        out << YAML::Key << "signals" << YAML::Value << YAML::BeginMap;
        for (const auto& [name, value] : a.signals)
            out << YAML::Key << name << YAML::Value << value;
        out << YAML::EndMap;
    }
    out << YAML::Key << "paused" << YAML::Value << a.paused;
    out << YAML::EndMap;
}

static void emit_flows(YAML::Emitter& out, const char* key, const std::vector<FlowSpec>& flows) {
    if (!flows.empty()) {
        out << YAML::Key << key << YAML::Value << YAML::BeginSeq;
        for (const auto& f : flows) {
            out << YAML::BeginMap;
            out << YAML::Key << "name"    << YAML::Value << f.name;
            out << YAML::Key << "enabled" << YAML::Value << f.enabled;
            out << YAML::Key << "step_ms" << YAML::Value << f.step_ms;
            out << YAML::Key << "nodes" << YAML::Value << YAML::BeginSeq;
            for (const auto& n : f.nodes) {
                out << YAML::BeginMap;
                out << YAML::Key << "id"   << YAML::Value << n.id;
                out << YAML::Key << "type" << YAML::Value << n.type;
                out << YAML::Key << "name" << YAML::Value << n.name;
                out << YAML::Key << "x" << YAML::Value << n.x;
                out << YAML::Key << "y" << YAML::Value << n.y;
                if (n.type == "can_in" || n.type == "can_out") {
                    out << YAML::Key << "interface" << YAML::Value << n.interface;
                    out << YAML::Key << "msg"       << YAML::Value << n.msg;
                    out << YAML::Key << "signals"   << YAML::Value << YAML::Flow << n.signals;
                    if (n.type == "can_out") {
                        out << YAML::Key << "period_ms" << YAML::Value << n.period_ms;
                        if (!n.sig_values.empty())
                            out << YAML::Key << "sig_values" << YAML::Value << YAML::Flow << n.sig_values;
                    }
                } else if (n.type == "lua") {
                    if (!n.interface.empty())
                        out << YAML::Key << "interface" << YAML::Value << n.interface;
                    if (!n.signals.empty())
                        out << YAML::Key << "signals" << YAML::Value << YAML::Flow << n.signals;
                    out << YAML::Key << "code" << YAML::Value << YAML::Literal << n.code;
                } else if (n.type == "generator") {
                    out << YAML::Key << "wave"      << YAML::Value << n.wave;
                    out << YAML::Key << "amplitude" << YAML::Value << n.amplitude;
                    out << YAML::Key << "offset"    << YAML::Value << n.offset;
                    out << YAML::Key << "frequency" << YAML::Value << n.frequency;
                    out << YAML::Key << "phase"     << YAML::Value << n.phase;
                    out << YAML::Key << "duty"      << YAML::Value << n.duty;
                } else if (n.type == "csv") {
                    out << YAML::Key << "signals" << YAML::Value << YAML::Flow << n.signals;
                    out << YAML::Key << "path"    << YAML::Value << n.path;
                } else if (n.type == "const") {
                    out << YAML::Key << "offset" << YAML::Value << n.offset;
                } else if (n.type == "compare") {
                    out << YAML::Key << "op" << YAML::Value << n.op;
                } else if (n.type == "movavg" || n.type == "fft") {
                    out << YAML::Key << "window" << YAML::Value << n.window;
                } else if (n.type == "diff") {
                    out << YAML::Key << "derivative" << YAML::Value << n.derivative;
                }
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
            out << YAML::Key << "links" << YAML::Value << YAML::BeginSeq;
            for (const auto& l : f.links)
                out << YAML::Flow << YAML::BeginSeq << l.from_node << l.from_port << l.to_node << l.to_port << YAML::EndSeq;
            out << YAML::EndSeq;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
    }
}

std::string config_to_yaml(const Config& config) {
    YAML::Emitter out;
    out.SetIndent(2);
    out << YAML::BeginMap;

    out << YAML::Key << "virtual" << YAML::Value << config.virtual_can;
    if (!config.log_file_path.empty())
        out << YAML::Key << "log_file_path" << YAML::Value << config.log_file_path;
    if (!config.log_viewer.empty())
        out << YAML::Key << "log_viewer" << YAML::Value << config.log_viewer;

    out << YAML::Key << "gui" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "scale" << YAML::Value << config.gui.scale;
    out << YAML::Key << "theme" << YAML::Value << config.gui.theme;
    if (config.gui.autosave_s > 0)
        out << YAML::Key << "autosave_s" << YAML::Value << config.gui.autosave_s;
    out << YAML::EndMap;

    out << YAML::Key << "logging" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "format"          << YAML::Value << config.logging.format;
    out << YAML::Key << "raw"             << YAML::Value << config.logging.raw;
    out << YAML::Key << "update_ratio"    << YAML::Value << config.logging.update_ratio;
    out << YAML::Key << "min_interval_ms" << YAML::Value << config.logging.min_interval_ms;
    out << YAML::Key << "flush_s"         << YAML::Value << config.logging.flush_s;
    out << YAML::EndMap;

    out << YAML::Key << "interfaces" << YAML::Value << YAML::BeginMap;
    for (const auto& iface : config.interfaces) {
        out << YAML::Key << iface.name << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "dbc" << YAML::Value << iface.dbc;
        if (iface.baudrate > 0)
            out << YAML::Key << "baudrate" << YAML::Value << iface.baudrate;
        if (!iface.actions.empty()) {
            out << YAML::Key << "actions" << YAML::Value << YAML::BeginSeq;
            for (const auto& a : iface.actions)
                emit_action(out, a);
            out << YAML::EndSeq;
        }
        out << YAML::EndMap;
    }
    out << YAML::EndMap;

    out << YAML::Key << "graphs" << YAML::Value << YAML::BeginSeq;
    for (const auto& g : config.graphs) {
        out << YAML::BeginMap;
        out << YAML::Key << "name"     << YAML::Value << g.name;
        out << YAML::Key << "window_s" << YAML::Value << g.window_s;
        out << YAML::Key << "auto_y"   << YAML::Value << g.auto_y;
        out << YAML::Key << "signals"  << YAML::Value << YAML::BeginSeq;
        for (const auto& s : g.signals) {
            out << YAML::BeginMap;
            if (!s.interface.empty())
                out << YAML::Key << "interface" << YAML::Value << s.interface;
            if (!s.msg.empty())
                out << YAML::Key << "msg" << YAML::Value << s.msg;
            out << YAML::Key << "signal" << YAML::Value << s.signal;
            if (s.y_axis != 0)
                out << YAML::Key << "y_axis" << YAML::Value << s.y_axis;
            if (s.y_min) out << YAML::Key << "y_min" << YAML::Value << *s.y_min;
            if (s.y_max) out << YAML::Key << "y_max" << YAML::Value << *s.y_max;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;

    if (!config.guards.empty()) {
        out << YAML::Key << "guards" << YAML::Value << YAML::BeginSeq;
        for (const auto& g : config.guards) {
            out << YAML::BeginMap;
            out << YAML::Key << "name"      << YAML::Value << g.name;
            out << YAML::Key << "interface" << YAML::Value << g.interface;
            out << YAML::Key << "msg"       << YAML::Value << g.msg;
            out << YAML::Key << "signal"    << YAML::Value << g.signal;
            out << YAML::Key << "script"    << YAML::Value << g.script;
            out << YAML::Key << "enabled"   << YAML::Value << g.enabled;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
    }

    if (!config.dbc_history.empty()) {
        out << YAML::Key << "dbc_history" << YAML::Value << YAML::BeginSeq;
        for (const auto& d : config.dbc_history)
            out << d;
        out << YAML::EndSeq;
    }

    if (!config.debug_history.empty()) {
        out << YAML::Key << "debug_history" << YAML::Value << YAML::BeginSeq;
        for (const auto& d : config.debug_history) {
            out << YAML::BeginMap;
            out << YAML::Key << "name"  << YAML::Value << d.name;
            out << YAML::Key << "log"   << YAML::Value << d.log;
            out << YAML::Key << "saved" << YAML::Value << d.saved;
            if (!d.dbcs.empty()) {
                out << YAML::Key << "dbcs" << YAML::Value << YAML::BeginMap;
                for (const auto& [iface, dbc] : d.dbcs)
                    out << YAML::Key << iface << YAML::Value << dbc;
                out << YAML::EndMap;
            }
            out << YAML::Key << "tabs" << YAML::Value << YAML::BeginSeq;
            for (const auto& t : d.tabs) {
                out << YAML::BeginMap;
                out << YAML::Key << "name" << YAML::Value << t.name;
                out << YAML::Key << "plots" << YAML::Value << YAML::BeginSeq;
                for (const auto& p : t.plots)
                    out << YAML::Flow << p;
                out << YAML::EndSeq << YAML::EndMap;
            }
            out << YAML::EndSeq << YAML::EndMap;
        }
        out << YAML::EndSeq;
    }

    emit_flows(out, "flows", config.flows);
    emit_flows(out, "debug_flows", config.debug_flows);

    out << YAML::EndMap;

    if (!out.good())
        throw std::runtime_error(std::string("YAML emitter error: ") + out.GetLastError());
    return out.c_str();
}

void save_config(const Config& config, const std::string& path) {
    const std::string yaml = config_to_yaml(config);

    // Write to a temp file first so a crash never leaves a half-written config.
    const std::filesystem::path target = path;
    const std::filesystem::path tmp    = target.string() + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f)
            throw std::runtime_error("Cannot write " + tmp.string());
        f << "# caneo configuration - written by caneo, comments are not preserved\n";
        f << yaml << '\n';
        if (!f)
            throw std::runtime_error("Error while writing " + tmp.string());
    }
    std::filesystem::rename(tmp, target);
}

// ── printing ────────────────────────────────────────────────────────────────

std::ostream& operator<<(std::ostream& os, const ActionSpec& spec) {
    os << spec.type << " on " << spec.interface << ": ";
    if (spec.type == "lua")
        os << spec.script;
    else
        os << spec.msg;
    if (spec.type != "single")
        os << " every " << spec.period_ms << "ms";
    return os;
}

std::ostream& operator<<(std::ostream& os, const Config& config) {
    os << "Config(virtual=" << config.virtual_can << ", interfaces=[";
    for (std::size_t i = 0; i < config.interfaces.size(); ++i)
        os << (i ? ", " : "") << config.interfaces[i].name;
    os << "], graphs=" << config.graphs.size() << ", guards=" << config.guards.size()
       << ", flows=" << config.flows.size() << ")";
    return os;
}

std::ostream& operator<<(std::ostream& os, const GuardSpec& spec) {
    return os << "guard " << spec.name << " on " << spec.interface << ":"
              << spec.msg << "." << spec.signal << " (" << spec.script << ")";
}

std::ostream& operator<<(std::ostream& os, const FlowSpec& spec) {
    return os << "flow " << spec.name << " (" << spec.nodes.size() << " blocks, "
              << spec.links.size() << " links, every " << spec.step_ms << "ms)";
}
