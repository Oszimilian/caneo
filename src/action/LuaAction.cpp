#include "LuaAction.hpp"

#include "model/SignalStore.hpp"

#include <format>
#include <sol/sol.hpp>

static std::optional<double> to_double(const sol::object& obj) {
    if (obj.is<double>())    return obj.as<double>();
    if (obj.is<long long>()) return static_cast<double>(obj.as<long long>());
    if (obj.is<bool>())      return obj.as<bool>() ? 1.0 : 0.0;
    return std::nullopt;
}

LuaAction::LuaAction(boost::asio::io_context&     io,
                     std::string                  interface,
                     SendFn                       send_fn,
                     std::chrono::milliseconds    period,
                     const std::filesystem::path& script_path,
                     ModelMap                     models,
                     const SignalStore*           store)
    : Action(io, std::move(interface), 0, script_path.stem().string(), {}, std::move(send_fn))
    , period_(std::max(period, std::chrono::milliseconds(1)))
    , models_(std::move(models))
    , store_(store)
    , resume_time_(std::chrono::steady_clock::now())
{
    loaded_ = load(script_path);
}

// Out-of-line so sol::state is complete here.
LuaAction::~LuaAction() = default;

void LuaAction::register_api() {
    auto& lua = *lua_;

    lua["interface"] = interface_;
    lua["period_ms"] = period_.count();

    lua.set_function("print", [this](sol::variadic_args args) {
        std::string line;
        sol::function tostring = (*lua_)["tostring"];
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) line += '\t';
            line += tostring(args[i]).get<std::string>();
        }
        log_->append(std::move(line));
    });

    lua.set_function("get_signal",
        [this](const std::string& iface, const std::string& msg, const std::string& sig) -> double {
            return store_ ? store_->get(iface, msg, sig).value_or(0.0) : 0.0;
        });

    lua.set_function("send",
        [this](const std::string& msg, sol::optional<sol::table> sigs,
               sol::optional<std::string> iface_opt) {
            const std::string iface = iface_opt.value_or(interface_);
            const auto mit = models_.find(iface);
            if (mit == models_.end() || !mit->second)
                throw std::runtime_error(std::format("send: no DBC loaded for interface '{}'", iface));
            const SendModel& model = *mit->second;

            const int msg_idx = model.find_message(msg);
            if (msg_idx < 0)
                throw std::runtime_error(std::format("send: unknown message '{}' on {}", msg, iface));
            const auto& m = model.messages()[msg_idx];

            auto& values = values_[{iface, msg_idx}];
            values.resize(m.signals.size(), 0.0);

            if (sigs) {
                for (const auto& [key, val] : *sigs) {
                    if (!key.is<std::string>())
                        continue;
                    const auto name = key.as<std::string>();
                    const int sig_idx = model.find_signal(msg_idx, name);
                    if (sig_idx < 0)
                        throw std::runtime_error(std::format("send: message '{}' has no signal '{}'", msg, name));
                    const auto v = to_double(val);
                    if (!v)
                        throw std::runtime_error(std::format("send: value of '{}' is not a number", name));
                    values[sig_idx] = *v;
                }
            }
            send_fn_(iface, m.id, model.encode_values(msg_idx, values));
            last_sent_ = std::chrono::steady_clock::now();
            ever_sent_ = true;
        });

    lua.set_function("send_raw",
        [this](long long id, sol::table bytes, sol::optional<std::string> iface_opt) {
            std::vector<uint8_t> data;
            for (std::size_t i = 1; i <= bytes.size() && i <= 64; ++i)
                data.push_back(static_cast<uint8_t>(bytes.get<long long>(i) & 0xFF));
            send_fn_(iface_opt.value_or(interface_), static_cast<uint64_t>(id), data);
            last_sent_ = std::chrono::steady_clock::now();
            ever_sent_ = true;
        });
}

bool LuaAction::load(const std::filesystem::path& script_path) {
    lua_ = std::make_unique<sol::state>();
    lua_->open_libraries(sol::lib::base, sol::lib::math, sol::lib::table,
                         sol::lib::string, sol::lib::os);
    register_api();

    const auto result = lua_->safe_script_file(script_path.string(), sol::script_pass_on_error);
    if (!result.valid()) {
        const sol::error err = result;
        log_->set_error(err.what());
        return false;
    }
    if (!(*lua_)["tick"].is<sol::function>()) {
        log_->set_error("script defines no tick(t) function: " + script_path.string());
        return false;
    }
    if ((*lua_)["init"].is<sol::function>()) {
        sol::protected_function init = (*lua_)["init"];
        const auto r = init();
        if (!r.valid()) {
            const sol::error err = r;
            log_->set_error(std::string("init(): ") + err.what());
            return false;
        }
    }
    return true;
}

double LuaAction::elapsed_seconds() const {
    return accumulated_.count() +
           std::chrono::duration<double>(std::chrono::steady_clock::now() - resume_time_).count();
}

void LuaAction::pause() {
    if (!paused_)
        accumulated_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - resume_time_);
    Action::pause();
}

void LuaAction::resume() {
    resume_time_ = std::chrono::steady_clock::now();
    if (loaded_)
        log_->set_error({});
    Action::resume();
}

void LuaAction::fail(const std::string& what) {
    log_->set_error(what);
    pause();
    if (on_fired_) on_fired_();   // refresh the snapshot so the GUI sees the error
}

void LuaAction::schedule() {
    if (!loaded_) {
        // Keep the action around (so its spec can be edited and saved) but
        // never run a script that failed to load.
        if (!paused_)
            paused_at_ = std::chrono::steady_clock::now();
        paused_ = true;
        return;
    }
    timer_.expires_after(period_);
    timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) return;
        tick();
        if (!paused_)
            schedule();
    });
}

void LuaAction::tick() {
    sol::protected_function fn = (*lua_)["tick"];
    const auto result = fn(elapsed_seconds());
    if (!result.valid()) {
        const sol::error err = result;
        fail(std::string("tick(): ") + err.what());
        return;
    }
    if (on_fired_) on_fired_();
}
