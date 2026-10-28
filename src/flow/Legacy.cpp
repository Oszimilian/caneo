#include "Legacy.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sol/sol.hpp>
#include <sstream>

namespace {

std::optional<std::string> read_file(const std::string& path)
{
    std::ifstream f(expand_path(path));
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Output names of a guard script: keys of the table that check(0, 0) returns.
std::vector<std::string> guard_outputs(const GuardSpec& g, const std::string& code)
{
    std::vector<std::string> names;
    try {
        sol::state lua;
        lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::table, sol::lib::string, sol::lib::os);
        lua["interface"] = g.interface;
        lua["msg"]       = g.msg;
        lua["signal"]    = g.signal;
        lua.set_function("print", [](sol::variadic_args) {});
        lua.set_function("get_signal", [](const std::string&, const std::string&, const std::string&) { return 0.0; });
        if (lua.safe_script(code, sol::script_pass_on_error).valid() && lua["check"].is<sol::function>()) {
            sol::protected_function check = lua["check"];
            const auto r = check(0.0, 0.0);
            if (r.valid() && r.get_type() == sol::type::table) {
                const sol::table tb = r;
                for (const auto& kv : tb)
                    if (kv.first.is<std::string>()) names.push_back(kv.first.as<std::string>());
            }
        }
    } catch (const std::exception&) {}
    std::ranges::sort(names);
    if (names.empty()) names.push_back("out");
    return names;
}

FlowNodeSpec node(int id, const char* type, std::string name, float x, float y)
{
    FlowNodeSpec n;
    n.id   = id;
    n.type = type;
    n.name = std::move(name);
    n.x    = x;
    n.y    = y;
    return n;
}

std::string stem(const std::string& path)
{
    return std::filesystem::path(path).stem().string();
}

FlowSpec from_guard(const GuardSpec& g)
{
    FlowSpec f;
    const std::string name = g.name.empty() ? stem(g.script) : g.name;
    f.name    = "Guard: " + name;
    f.enabled = g.enabled;
    f.step_ms = 10;

    std::string code;
    if (const auto text = read_file(g.script))
        code = *text + "\n\n-- added by the conversion of the guard: step() calls check()\nfunction step(u, t)\n    return check(u, t)\nend\n";
    else
        code = std::format("-- could not read the guard script '{}'\nfunction step(u, t)\n    return 0\nend\n", g.script);
    const auto outputs = guard_outputs(g, code);

    auto in = node(1, "can_in", "CAN In", 0, 0);
    in.interface = g.interface;
    in.msg       = g.msg;
    in.signals   = {g.signal};
    auto lua = node(2, "lua", name, 320, 0);
    lua.interface = g.interface;
    lua.signals   = outputs;
    lua.code      = code;
    f.nodes       = {in, lua};
    f.links.push_back({1, 0, 2, 0});
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const int id = 3 + static_cast<int>(i);
        f.nodes.push_back(node(id, "scope", name + "/" + outputs[i], 700, static_cast<float>(i) * 90));
        f.links.push_back({2, static_cast<int>(i), id, 0});
    }
    return f;
}

FlowSpec from_action(const ActionSpec& a)
{
    FlowSpec f;
    f.name    = "Action: " + (!a.name.empty() ? a.name : a.type == "lua" ? stem(a.script) : a.msg);
    f.enabled = !a.paused;
    f.step_ms = std::max(1, a.period_ms);

    if (a.type == "lua") {
        auto lua = node(1, "lua", f.name, 0, 0);
        lua.interface = a.interface;
        std::string code = std::format("period_ms = {}\n", a.period_ms);
        if (const auto text = read_file(a.script))
            code += *text + "\n\n-- added by the conversion of the action: step() calls tick()\nfunction step(u, t)\n    tick(t)\n    return 0\nend\n";
        else
            code += std::format("-- could not read the action script '{}'\nfunction step(u, t)\n    return 0\nend\n", a.script);
        lua.code = std::move(code);
        f.nodes  = {lua};
        return f;
    }

    auto out = node(a.type == "sin" ? 2 : 1, "can_out", "CAN Out", a.type == "sin" ? 300.0f : 0.0f, 0);
    out.interface = a.interface;
    out.msg       = a.msg;
    out.period_ms = a.period_ms;
    for (const auto& [sig, val] : a.signals) {
        if (a.type == "sin" && sig == a.signal) continue;
        out.signals.push_back(sig);
        out.sig_values.push_back(val);
    }
    if (a.type == "sin") {
        auto gen = node(1, "generator", "Sine", 0, 0);
        gen.wave      = "sin";
        gen.amplitude = a.amplitude;
        gen.offset    = a.offset;
        gen.frequency = 1000.0 / std::max(1, a.sin_period_ms);
        out.signals.push_back(a.signal);
        out.sig_values.push_back(a.offset);
        f.nodes = {gen, out};
        f.links.push_back({1, 0, 2, static_cast<int>(out.signals.size()) - 1});
    } else {
        f.nodes = {out};
    }
    return f;
}

} // namespace

std::vector<std::string> migrate_legacy(Config& config)
{
    std::vector<std::string> notes;
    for (auto& iface : config.interfaces) {
        for (const auto& a : iface.actions) {
            if (a.type == "single" || a.type.empty()) continue;
            config.flows.push_back(from_action(a));
            notes.push_back(config.flows.back().name);
        }
        iface.actions.clear();
    }
    for (const auto& g : config.guards) {
        config.flows.push_back(from_guard(g));
        notes.push_back(config.flows.back().name);
    }
    config.guards.clear();
    return notes;
}
