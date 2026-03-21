#include "GraphPreset.hpp"

#include <dbcppp/Network.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

// Build msg_name → {id, signal_name → {y_min, y_max}} lookup from DBC.
struct SigInfo { double y_min; double y_max; };
struct MsgInfo  { uint32_t id; std::unordered_map<std::string, SigInfo> signals; };

static std::unordered_map<std::string, MsgInfo> build_dbc_index(const std::string& dbc_path)
{
    std::ifstream f(dbc_path);
    if (!f.is_open())
        throw std::runtime_error("GraphPreset: cannot open DBC: " + dbc_path);

    auto network = dbcppp::INetwork::LoadDBCFromIs(f);
    if (!network)
        throw std::runtime_error("GraphPreset: cannot parse DBC: " + dbc_path);

    std::unordered_map<std::string, MsgInfo> index;
    for (const dbcppp::IMessage& msg : network->Messages()) {
        MsgInfo mi;
        mi.id = static_cast<uint32_t>(msg.Id());
        for (const dbcppp::ISignal& sig : msg.Signals())
            mi.signals[std::string(sig.Name())] = {sig.Minimum(), sig.Maximum()};
        index[std::string(msg.Name())] = std::move(mi);
    }
    return index;
}

void load_graph_preset(const InterfaceConfig& cfg, GraphWindow& graph_window)
{
    if (cfg.graphs_file.empty())
        return;

    YAML::Node root;
    try {
        root = YAML::LoadFile(cfg.graphs_file);
    } catch (const std::exception& e) {
        throw std::runtime_error(
            std::string("Failed to load graphs preset '") + cfg.graphs_file + "': " + e.what());
    }

    if (!root.IsSequence())
        throw std::runtime_error("Graphs preset must be a YAML sequence");

    auto dbc_index = build_dbc_index(cfg.dbc);

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

            auto mit = dbc_index.find(msg_name);
            if (mit == dbc_index.end())
                continue;

            const MsgInfo& mi = mit->second;
            auto sit = mi.signals.find(sig_name);

            double y_min = 0.0;
            double y_max = 0.0;
            if (sit != mi.signals.end()) {
                y_min = sit->second.y_min;
                y_max = sit->second.y_max;
            }
            if (sig_node["y_min"]) y_min = sig_node["y_min"].as<double>();
            if (sig_node["y_max"]) y_max = sig_node["y_max"].as<double>();

            const int y_axis = sig_node["y_axis"] ? sig_node["y_axis"].as<int>() : 0;

            graph_window.add_signal(graph_idx, cfg.name, mi.id, sig_name, y_min, y_max, y_axis,
                                    msg_name + "/" + sig_name);
        }
    }
}
