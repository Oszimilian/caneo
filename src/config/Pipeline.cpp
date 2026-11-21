#include "Pipeline.hpp"

#include <algorithm>
#include <map>
#include <set>

bool pipeline_is_device(const PipelineNodeSpec& n)
{
    return n.type == "hw_socketcan" || n.type == "virtual_socketcan" || n.type == "grpc_socketcan";
}

int pipeline_in_ports(const PipelineNodeSpec& n)
{
    return pipeline_is_device(n) ? 0 : 1;
}

int pipeline_out_ports(const PipelineNodeSpec& n)
{
    if (pipeline_is_device(n)) return static_cast<int>(n.channels.size());
    if (n.type == "baudrate" || n.type == "decode") return 1;   // xcp: input only
    return 0;
}

PipelineSpec pipeline_from_interfaces(const Config& cfg)
{
    PipelineSpec p;
    int next = 1;
    auto add = [&](const char* type, float x, float y) -> PipelineNodeSpec& {
        PipelineNodeSpec n;
        n.id = next++;
        n.type = type;
        n.x = x;
        n.y = y;
        return p.nodes.emplace_back(std::move(n));
    };
    auto link = [&](int a, int ap, int b, int bp) { p.links.push_back({a, ap, b, bp}); };

    constexpr float DX = 320.0f, DY = 150.0f;
    const bool any_live = std::ranges::any_of(cfg.interfaces, [](const auto& i) { return i.live; });
    const bool any_log  = std::ranges::any_of(cfg.interfaces, [](const auto& i) { return i.log; });

    int live = 0, mcap = 0, blf = 0;
    const float sink_x = 3 * DX;
    float sink_y = 0.0f;
    if (any_live) { live = add("live", sink_x, sink_y).id; sink_y += DY; }
    if (any_log) {
        if (cfg.logging.mcap()) { mcap = add("log", sink_x, sink_y).id; sink_y += DY; }
        if (cfg.logging.blf())  { auto& n = add("log", sink_x, sink_y); n.format = "blf"; blf = n.id; sink_y += DY; }
    }

    float y = 0.0f;
    for (const auto& i : cfg.interfaces) {
        if (i.is_remote()) {   // remote channel: gRPC block, no baudrate (the server sets the bus up)
            auto& dev = add("grpc_socketcan", 0, y);
            dev.address  = i.grpc;
            dev.channels = {i.remote_name()};
            if (i.name.size() > i.remote_name().size() && i.name.ends_with(i.remote_name()))
                dev.prefix = i.name.substr(0, i.name.size() - i.remote_name().size());
            int last = dev.id;
            if (!i.dbc.empty()) {
                auto& dc = add("decode", 2 * DX, y);
                dc.dbc = i.dbc;
                link(last, 0, dc.id, 0);
                last = dc.id;
            }
            if (i.live && live) link(last, 0, live, 0);
            if (i.log && mcap)  link(last, 0, mcap, 0);
            if (i.log && blf)   link(last, 0, blf, 0);
            y += DY;
            continue;
        }
        const bool vir = cfg.virtual_can || i.virtual_can;
        auto& dev = add(vir ? "virtual_socketcan" : "hw_socketcan", 0, y);
        dev.channels = {i.name};
        const int dev_id = dev.id;
        auto& bd = add("baudrate", DX, y);
        bd.virtual_can = vir;
        if (i.baudrate > 0) bd.baudrate = i.baudrate;
        bd.fd = i.fd;
        if (i.data_baudrate > 0) bd.data_baudrate = i.data_baudrate;
        const int bd_id = bd.id;
        link(dev_id, 0, bd_id, 0);
        int last = bd_id;
        if (!i.dbc.empty()) {
            auto& dc = add("decode", 2 * DX, y);
            dc.dbc = i.dbc;
            link(last, 0, dc.id, 0);
            last = dc.id;
        }
        if (i.live && live) link(last, 0, live, 0);
        if (i.log && mcap)  link(last, 0, mcap, 0);
        if (i.log && blf)   link(last, 0, blf, 0);
        y += DY;
    }
    return p;
}

void derive_interfaces(Config& cfg)
{
    const auto& p = cfg.pipeline;
    std::map<int, const PipelineNodeSpec*> by_id;
    for (const auto& n : p.nodes) by_id[n.id] = &n;

    std::vector<InterfaceConfig> out;
    std::vector<XcpTargetConfig> xcp;
    std::set<std::string> blf_or_mcap;
    bool any_log_node = false, mcap = false, blf = false;
    std::set<std::string> seen;

    for (const auto& dev : p.nodes) {
        if (!pipeline_is_device(dev)) continue;
        const bool remote = dev.type == "grpc_socketcan";
        if (remote && (!cfg.grpc.enabled || dev.address.empty())) continue;   // gRPC switched off in Settings
        for (std::size_t port = 0; port < dev.channels.size(); ++port) {
            InterfaceConfig ic;
            ic.name = dev.channels[port];
            ic.live = ic.log = false;
            ic.virtual_can = dev.type == "virtual_socketcan";
            if (remote && !ic.name.empty()) {
                ic.grpc   = dev.address;
                ic.remote = ic.name;
                ic.name   = dev.prefix + ic.name;
            }
            if (ic.name.empty() || seen.contains(ic.name)) continue;

            // follow the links downstream
            bool connected = false, baud_set = false, dbc_set = false;
            std::vector<std::pair<int, int>> todo;   // node, port
            std::set<int> visited;
            for (const auto& l : p.links)
                if (l.from_node == dev.id && l.from_port == static_cast<int>(port)) todo.emplace_back(l.to_node, 0);
            while (!todo.empty()) {
                const int id = todo.back().first;
                todo.pop_back();
                if (!visited.insert(id).second) continue;
                const auto it = by_id.find(id);
                if (it == by_id.end()) continue;
                const auto& n = *it->second;
                connected = true;
                if (n.type == "baudrate" && !baud_set && !remote) {   // remote: the server owns the bus
                    baud_set = true;
                    ic.baudrate = n.virtual_can ? 0 : n.baudrate;
                    if (n.virtual_can) ic.virtual_can = true;
                    ic.fd            = n.fd;
                    ic.data_baudrate = n.fd && !n.virtual_can ? n.data_baudrate : 0;
                } else if (n.type == "decode" && !dbc_set && !n.dbc.empty()) {
                    dbc_set = true;
                    ic.dbc  = n.dbc;
                } else if (n.type == "live") {
                    ic.live = true;
                } else if (n.type == "log") {
                    ic.log = true;
                } else if (n.type == "xcp") {
                    XcpTargetConfig t;
                    t.name       = n.name.empty() ? "XCP " + ic.name : n.name;
                    t.interface  = ic.name;
                    t.tx_id      = n.xcp_tx_id;
                    t.rx_id      = n.xcp_rx_id;
                    t.addr_ext   = n.xcp_addr_ext;
                    t.timeout_ms = n.xcp_timeout_ms;
                    t.pad        = n.xcp_pad;
                    xcp.push_back(std::move(t));
                }
                for (const auto& l : p.links)
                    if (l.from_node == id) todo.emplace_back(l.to_node, 0);
            }
            if (!connected) continue;
            for (auto& t : xcp)
                if (t.interface == ic.name) t.fd = ic.fd;
            seen.insert(ic.name);
            out.push_back(std::move(ic));
        }
    }

    // keep the actions of interfaces that still exist
    for (auto& ic : out)
        for (const auto& old : cfg.interfaces)
            if (old.name == ic.name) ic.actions = old.actions;

    // log format = union of the log blocks that receive data
    for (const auto& n : p.nodes) {
        if (n.type != "log") continue;
        any_log_node = true;
        const bool fed = std::ranges::any_of(p.links, [&](const auto& l) { return l.to_node == n.id; });
        if (!fed) continue;
        (n.format == "blf" ? blf : mcap) = true;
    }
    if (any_log_node && (mcap || blf))
        cfg.logging.format = mcap && blf ? "both" : blf ? "blf" : "mcap";

    cfg.interfaces  = std::move(out);
    cfg.xcp_targets = std::move(xcp);
    cfg.virtual_can = false;
}

void sync_pipeline(Config& cfg)
{
    if (cfg.pipeline.nodes.empty()) {
        if (cfg.interfaces.empty()) return;
        // old file with remote channels in `interfaces:` -> keep them working
        if (std::ranges::any_of(cfg.interfaces, &InterfaceConfig::is_remote)) cfg.grpc.enabled = true;
        cfg.pipeline = pipeline_from_interfaces(cfg);
    }
    derive_interfaces(cfg);
}
