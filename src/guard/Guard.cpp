#include "Guard.hpp"

#include "model/SignalStore.hpp"

#include <algorithm>
#include <sol/sol.hpp>

const char* Guard::TEMPLATE = R"(-- caneo guard
--
-- check(value, t) runs for every received frame of the guarded message.
--   value   current value of the guarded signal
--   t       seconds since caneo started
-- Return the outputs: a number, a boolean, or a table {name = value, ...}.
-- They are shown in Trace > Guards, can be plotted and are logged as
-- guard/<guard name>/<output> while logging is active.
--
--   get_signal(iface, msg, signal)   last value of any other signal (0 if none)
--   print(...)                       guard log (shown in the Script Editor)
--   interface, msg, signal           what this guard watches

local limit = 100

function check(value, t)
    return {
        too_high = value > limit,     -- booleans become 1 / 0
        margin   = limit - value,
    }
end
)";

// ─── GuardState ─────────────────────────────────────────────────────────────

void GuardState::set(double input, const std::vector<std::pair<std::string, double>>& outputs)
{
    std::lock_guard lock(mutex_);
    values_.input = input;
    for (const auto& [name, v] : outputs) {
        auto it = std::ranges::find(values_.outputs, name, &std::pair<std::string, double>::first);
        if (it != values_.outputs.end()) it->second = v;
        else                             values_.outputs.emplace_back(name, v);
    }
    ++values_.count;
    values_.last_run = std::chrono::steady_clock::now();
}

GuardState::Values GuardState::values() const
{
    std::lock_guard lock(mutex_);
    return values_;
}

void GuardState::reset()
{
    std::lock_guard lock(mutex_);
    values_ = {};
}

// ─── Guard ──────────────────────────────────────────────────────────────────

static std::optional<double> to_double(const sol::object& obj)
{
    if (obj.is<bool>())      return obj.as<bool>() ? 1.0 : 0.0;
    if (obj.is<double>())    return obj.as<double>();
    if (obj.is<long long>()) return static_cast<double>(obj.as<long long>());
    return std::nullopt;
}

Guard::Guard(GuardSpec spec, const SignalStore* store)
    : spec_(std::move(spec)), store_(store)
{
    if (spec_.name.empty())
        spec_.name = std::filesystem::path(spec_.script).stem().string();
    loaded_ = load();
}

Guard::~Guard() = default;

bool Guard::load()
{
    lua_ = std::make_unique<sol::state>();
    lua_->open_libraries(sol::lib::base, sol::lib::math, sol::lib::table,
                         sol::lib::string, sol::lib::os);
    auto& lua = *lua_;
    lua["interface"] = spec_.interface;
    lua["msg"]       = spec_.msg;
    lua["signal"]    = spec_.signal;

    lua.set_function("print", [this](sol::variadic_args args) {
        std::string line;
        sol::function tostring = (*lua_)["tostring"];
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) line += '\t';
            line += tostring(args[i]).get<std::string>();
        }
        state_->log.append(std::move(line));
    });
    lua.set_function("get_signal",
        [this](const std::string& iface, const std::string& msg, const std::string& sig) -> double {
            return store_ ? store_->get(iface, msg, sig).value_or(0.0) : 0.0;
        });

    if (spec_.script.empty()) {
        state_->log.set_error("no script set");
        return false;
    }
    const auto result = lua.safe_script_file(expand_path(spec_.script).string(), sol::script_pass_on_error);
    if (!result.valid()) {
        const sol::error err = result;
        state_->log.set_error(err.what());
        return false;
    }
    if (!lua["check"].is<sol::function>()) {
        state_->log.set_error("script defines no check(value, t) function");
        return false;
    }
    if (lua["init"].is<sol::function>()) {
        sol::protected_function init = lua["init"];
        const auto r = init();
        if (!r.valid()) {
            const sol::error err = r;
            state_->log.set_error(std::string("init(): ") + err.what());
            return false;
        }
    }
    return true;
}

bool Guard::matches(const CanFrame& frame) const
{
    return frame.header().interface == spec_.interface && frame.msg_name() == spec_.msg;
}

void Guard::evaluate(const CanFrame& frame, double t, const OutputFn& out)
{
    if (!loaded_ || !spec_.enabled)
        return;

    const auto& sigs = frame.decoded();
    const auto  sig  = std::ranges::find(sigs, spec_.signal, &DecodedSignal::name);
    if (sig == sigs.end())
        return;
    const double value = sig->value;

    sol::protected_function check = (*lua_)["check"];
    const auto result = check(value, t);
    if (!result.valid()) {
        const sol::error err = result;
        state_->log.set_error(std::string("check(): ") + err.what());
        spec_.enabled = false;   // stop until the user re-enables it
        return;
    }

    outputs_.clear();
    const sol::object ret = result[0];
    if (ret.is<sol::table>()) {
        ret.as<sol::table>().for_each([&](const sol::object& key, const sol::object& val) {
            if (!key.is<std::string>()) return;
            if (const auto v = to_double(val))
                outputs_.emplace_back(key.as<std::string>(), *v);
        });
        // Lua tables have no order; keep the GUI stable.
        std::ranges::sort(outputs_, {}, &std::pair<std::string, double>::first);
    } else if (const auto v = to_double(ret)) {
        outputs_.emplace_back("output", *v);
    }

    state_->set(value, outputs_);
    if (out)
        for (const auto& [name, v] : outputs_)
            out(spec_.name, name, v);
}
