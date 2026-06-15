#include "GraphPreset.hpp"

#include <dbcppp/Network.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

struct SigInfo { double y_min; double y_max; };
struct MsgInfo  { uint32_t id; std::unordered_map<std::string, SigInfo> signals; };

using DbcIndex = std::unordered_map<std::string, MsgInfo>;

static DbcIndex build_dbc_index(const std::string& dbc_path)
{
    std::ifstream f(dbc_path);
    if (!f.is_open())
        throw std::runtime_error("GraphPreset: cannot open DBC: " + dbc_path);

    auto network = dbcppp::INetwork::LoadDBCFromIs(f);
    if (!network)
        throw std::runtime_error("GraphPreset: cannot parse DBC: " + dbc_path);

    DbcIndex index;
    for (const dbcppp::IMessage& msg : network->Messages()) {
        MsgInfo mi;
        mi.id = static_cast<uint32_t>(msg.Id());
        for (const dbcppp::ISignal& sig : msg.Signals())
            mi.signals[std::string(sig.Name())] = {sig.Minimum(), sig.Maximum()};
        index[std::string(msg.Name())] = std::move(mi);
    }
    return index;
}

void load_graph_preset(const Config& config, GraphWindow& graph_window)
{
    if (config.graphs_file.empty())
        return;

    // Build per-interface DBC index
    std::unordered_map<std::string, DbcIndex> iface_index;
    for (const auto& cfg : config.interfaces) {
        if (cfg.dbc.empty()) continue;
        iface_index[cfg.name] = build_dbc_index(cfg.dbc);
    }

    YAML::Node root;
    try {
        root = YAML::LoadFile(config.graphs_file);
    } catch (const std::exception& e) {
        throw std::runtime_error(
            std::string("Failed to load graphs preset '") + config.graphs_file + "': " + e.what());
    }

    if (!root.IsSequence())
        throw std::runtime_error("Graphs preset must be a YAML sequence");

    for (const auto& graph_node : root) {
        const std::string graph_name = graph_node["name"] ? graph_node["name"].as<std::string>() : "";
        const size_t graph_idx = graph_window.add_graph(graph_name);

        const auto& signals_node = graph_node["signals"];
        if (!signals_node || !signals_node.IsSequence())
            continue;

        for (const auto& sig_node : signals_node) {
            const std::string msg_name = sig_node["msg"]    ? sig_node["msg"].as<std::string>()    : "";
            const std::string sig_name = sig_node["signal"] ? sig_node["signal"].as<std::string>() : "";
            if (msg_name.empty() || sig_name.empty())
                continue;

            const std::string iface_hint = sig_node["interface"] ? sig_node["interface"].as<std::string>() : "";

            const MsgInfo* mi = nullptr;
            std::string    resolved_iface;

            auto try_iface = [&](const std::string& iface_name) -> bool {
                auto iit = iface_index.find(iface_name);
                if (iit == iface_index.end()) return false;
                auto mit = iit->second.find(msg_name);
                if (mit == iit->second.end()) return false;
                mi = &mit->second;
                resolved_iface = iface_name;
                return true;
            };

            if (!iface_hint.empty()) {
                if (!try_iface(iface_hint))
                    continue;
            } else {
                for (const auto& cfg : config.interfaces)
                    if (try_iface(cfg.name)) break;
            }
            if (!mi) continue;

            double y_min = 0.0, y_max = 0.0;
            if (auto sit = mi->signals.find(sig_name); sit != mi->signals.end()) {
                y_min = sit->second.y_min;
                y_max = sit->second.y_max;
            }
            if (sig_node["y_min"]) y_min = sig_node["y_min"].as<double>();
            if (sig_node["y_max"]) y_max = sig_node["y_max"].as<double>();

            const int y_axis = sig_node["y_axis"] ? sig_node["y_axis"].as<int>() : 0;

            graph_window.add_signal(graph_idx, resolved_iface, mi->id, sig_name,
                                    y_min, y_max, y_axis, msg_name + "/" + sig_name);
        }
    }
}
