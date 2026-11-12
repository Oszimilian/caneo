#include "OfflineFlow.hpp"

#include "flow/Flow.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <optional>
#include <random>
#include <sol/sol.hpp>
#include <unordered_map>

namespace {

constexpr std::size_t MAX_STEPS     = 3'000'000;
constexpr std::size_t MAX_LOG_LINES = 500;

// Sample-and-hold reader of one series; t must not go backwards.
struct Sampler {
    const LogDocument::Series* s = nullptr;
    std::size_t                pos = 0;

    double at(double t)
    {
        if (!s || s->t.empty()) return 0.0;
        while (pos + 1 < s->t.size() && s->t[pos + 1] <= t) ++pos;
        return s->v[pos];   // before the first sample: the first value
    }
};

struct Node {
    const FlowNodeSpec*              spec = nullptr;
    std::vector<double>              in, out;
    std::vector<std::pair<int, int>> sources;
    std::vector<Sampler>             samplers;   // can_in, one per signal
    std::unique_ptr<sol::state>      lua;
    sol::protected_function          step;
    LogDocument::Series*             scope_out = nullptr;
    MathState                        math;
    CsvTable*                        csv = nullptr;
};

std::optional<double> to_double(const sol::object& obj)
{
    if (obj.is<bool>())      return obj.as<bool>() ? 1.0 : 0.0;
    if (obj.is<double>())    return obj.as<double>();
    if (obj.is<long long>()) return static_cast<double>(obj.as<long long>());
    return std::nullopt;
}

} // namespace

OfflineFlowResult run_offline_flow(const FlowSpec& spec, const LogDocument::Data& data)
{
    OfflineFlowResult result;
    const auto fail = [&result](std::string what) {
        result.error = std::move(what);
        return std::move(result);
    };

    // label -> series of the log itself (not other flows' results)
    std::unordered_map<std::string, const LogDocument::Series*> by_label;
    for (const auto& s : data.series)
        if (s.iface != "flow") by_label[s.label] = &s;
    const auto sampler_for = [&by_label](const std::string& iface, const std::string& msg, const std::string& sig) {
        Sampler sm;
        if (const auto it = by_label.find(iface + "/" + msg + "/" + sig); it != by_label.end()) sm.s = it->second;
        return sm;
    };

    const std::size_t n_nodes = spec.nodes.size();
    result.outputs.reserve(n_nodes);   // scope_out pointers must stay valid
    result.tables.reserve(n_nodes);

    std::vector<std::unique_ptr<Node>> nodes;
    for (const auto& ns : spec.nodes) {
        auto node  = std::make_unique<Node>();
        node->spec = &ns;
        node->in.assign(static_cast<std::size_t>(flow_in_ports(ns)), 0.0);
        node->out.assign(static_cast<std::size_t>(flow_out_ports(ns)), 0.0);
        node->sources.assign(node->in.size(), {-1, -1});
        nodes.push_back(std::move(node));
    }
    const auto index_of = [&spec](int id) -> int {
        for (std::size_t i = 0; i < spec.nodes.size(); ++i)
            if (spec.nodes[i].id == id) return static_cast<int>(i);
        return -1;
    };

    std::vector<std::vector<std::size_t>> deps(n_nodes);
    for (const auto& l : spec.links) {
        const int from = index_of(l.from_node), to = index_of(l.to_node);
        if (from < 0 || to < 0) continue;
        auto& dst = *nodes[static_cast<std::size_t>(to)];
        if (l.to_port < 0 || l.to_port >= static_cast<int>(dst.in.size())) continue;
        if (l.from_port < 0 || l.from_port >= static_cast<int>(nodes[static_cast<std::size_t>(from)]->out.size())) continue;
        dst.sources[static_cast<std::size_t>(l.to_port)] = {from, l.from_port};
        deps[static_cast<std::size_t>(to)].push_back(static_cast<std::size_t>(from));
    }

    // dependency order; loops read the previous step
    std::vector<std::size_t> order;
    std::vector<int>         mark(n_nodes, 0);
    std::function<void(std::size_t)> visit = [&](std::size_t i) {
        if (mark[i]) return;
        mark[i] = 1;
        for (auto d : deps[i]) visit(d);
        mark[i] = 2;
        order.push_back(i);
    };
    for (std::size_t i = 0; i < n_nodes; ++i) visit(i);

    double cur_t = 0.0;
    std::unordered_map<std::string, Sampler> lua_samplers;

    for (std::size_t i = 0; i < n_nodes; ++i) {
        Node&               node = *nodes[i];
        const FlowNodeSpec& ns   = spec.nodes[i];
        const std::string   who  = std::format("block '{}': ", ns.name.empty() ? ns.type : ns.name);

        if (ns.type == "can_in") {
            for (const auto& sig : ns.signals) {
                node.samplers.push_back(sampler_for(ns.interface, ns.msg, sig));
                if (!node.samplers.back().s)
                    return fail(who + std::format("signal {}/{}/{} is not in the log", ns.interface, ns.msg, sig));
            }
        } else if (ns.type == "csv") {
            auto& tb   = result.tables.emplace_back();
            tb.node_id = ns.id;
            tb.columns = ns.signals;
            node.csv   = &tb;
        } else if (ns.type == "scope") {
            auto& out  = result.outputs.emplace_back();
            out.iface  = "flow";
            out.msg    = spec.name;
            out.signal = ns.name;
            out.label  = "flow/" + spec.name + "/" + ns.name;
            node.scope_out = &out;
        } else if (ns.type == "lua") {
            node.lua = std::make_unique<sol::state>();
            node.lua->open_libraries(sol::lib::base, sol::lib::math, sol::lib::table, sol::lib::string, sol::lib::os);
            Node* np = &node;
            node.lua->set_function("print", [&result, np](sol::variadic_args args) {
                std::string line;
                sol::function tostring = (*np->lua)["tostring"];
                for (std::size_t k = 0; k < args.size(); ++k) {
                    if (k) line += '\t';
                    line += tostring(args[k]).get<std::string>();
                }
                if (result.log.size() < MAX_LOG_LINES)
                    result.log.push_back(std::format("[{}] {}", np->spec->name, line));
            });
            node.lua->set_function("get_signal",
                [&](const std::string& iface, const std::string& msg, const std::string& sig) -> double {
                    const std::string key = iface + "/" + msg + "/" + sig;
                    auto it = lua_samplers.find(key);
                    if (it == lua_samplers.end()) it = lua_samplers.emplace(key, sampler_for(iface, msg, sig)).first;
                    return it->second.at(cur_t);
                });
            (*node.lua)["interface"] = ns.interface;
            node.lua->set_function("send", [](sol::variadic_args) {});       // nothing is sent offline
            node.lua->set_function("send_raw", [](sol::variadic_args) {});
            const auto r = node.lua->safe_script(ns.code, sol::script_pass_on_error);
            if (!r.valid()) {
                const sol::error e = r;
                return fail(who + e.what());
            }
            node.step = (*node.lua)["step"];
            if (!node.step.valid()) return fail(who + "script defines no step(u, t) function");
            if ((*node.lua)["init"].is<sol::function>()) {
                sol::protected_function init = (*node.lua)["init"];
                const auto ir = init();
                if (!ir.valid()) {
                    const sol::error e = ir;
                    return fail(who + "init(): " + e.what());
                }
            }
        }
    }

    const double dt = std::max(1, spec.step_ms) * 1e-3;
    const auto   steps = static_cast<std::size_t>(std::floor(data.duration / dt)) + 1;
    if (steps > MAX_STEPS)
        return fail(std::format("{} steps - raise 'step ms' (the log is {:.0f} s long)", steps, data.duration));
    for (auto& n : nodes)
        if (n->scope_out) { n->scope_out->t.reserve(steps); n->scope_out->v.reserve(steps); }
        else if (n->csv) { n->csv->t.reserve(steps); n->csv->data.reserve(steps * n->csv->columns.size()); }

    std::mt19937 rng{12345};
    for (std::size_t k = 0; k < steps; ++k) {
        const double t = static_cast<double>(k) * dt;
        cur_t          = t;
        for (const auto i : order) {
            Node&               node = *nodes[i];
            const FlowNodeSpec& ns   = spec.nodes[i];
            for (std::size_t p = 0; p < node.in.size(); ++p) {
                const auto [src, sp] = node.sources[p];
                node.in[p] = src < 0 ? 0.0 : nodes[static_cast<std::size_t>(src)]->out[static_cast<std::size_t>(sp)];
            }

            if (ns.type == "can_in") {
                for (std::size_t p = 0; p < node.out.size(); ++p) node.out[p] = node.samplers[p].at(t);
            } else if (ns.type == "generator") {
                node.out[0] = generator_value(ns, t, rng);
            } else if (ns.type == "const" || ns.type == "slider") {
                node.out[0] = ns.type == "slider" ? ns.value : ns.offset;
            } else if (ns.type == "button") {
                node.out[0] = 0.0;
            } else if (ns.type == "lua") {
                const auto r = node.in.size() == 1 ? node.step(node.in[0], t) : [&] {
                    std::vector<double> args = node.in;
                    args.push_back(t);
                    return node.step(sol::as_args(args));
                }();
                if (!r.valid()) {
                    const sol::error e = r;
                    return fail(std::format("block '{}': step() at t={:.3f}s: {}", ns.name, t, e.what()));
                }
                const sol::object ret = r[0];
                if (ret.is<sol::table>()) {
                    const sol::table tb = ret.as<sol::table>();
                    for (std::size_t p = 0; p < node.out.size(); ++p) {
                        sol::object o = p < ns.signals.size() ? tb[ns.signals[p]] : sol::object();
                        if (!o.valid() || o == sol::lua_nil) o = tb[p + 1];
                        if (const auto v = to_double(o)) node.out[p] = *v;
                    }
                } else if (const auto v = to_double(ret)) {
                    node.out[0] = *v;
                }
            } else if (is_math_block(ns.type)) {
                math_block_step(ns, node.math, node.in, node.out, dt);
            } else if (ns.type == "csv") {
                node.csv->t.push_back(t);
                node.csv->data.insert(node.csv->data.end(), node.in.begin(), node.in.end());
            } else if (ns.type == "scope") {
                node.scope_out->t.push_back(t);
                node.scope_out->v.push_back(node.in[0]);
            }
        }
    }
    return result;
}
