#include "ModelEngine.hpp"
#include "compat/print.hpp"

#include <sol/sol.hpp>

// Coerce a sol::object to double — handles both Lua floats and integers.
static std::optional<double> to_double(const sol::object& obj) {
    if (obj.is<double>())    return obj.as<double>();
    if (obj.is<long long>()) return static_cast<double>(obj.as<long long>());
    return std::nullopt;
}

// ─── Construction / Destruction ─────────────────────────────────────────────

ModelEngine::ModelEngine(const std::filesystem::path& script_path,
                         SignalStore& store,
                         Logger* logger)
    : store_(store), logger_(logger),
      script_name_(script_path.stem().string())
{
    lua_ = std::make_unique<sol::state>();
    lua_->open_libraries(sol::lib::base, sol::lib::math,
                         sol::lib::table, sol::lib::string);

    lua_->set_function("get_signal",
        [this](const std::string& iface,
               const std::string& msg,
               const std::string& signal) -> double {
            return store_.get(iface, msg, signal).value_or(0.0);
        });

    const auto result = lua_->safe_script_file(
        script_path.string(), sol::script_throw_on_error);

    if (!lua_->get<sol::object>("run").is<sol::function>())
        throw std::runtime_error(
            "ModelEngine: script has no run() function: " + script_path.string());

    std::println("ModelEngine: loaded '{}'", script_path.string());
}

// Out-of-line destructor so sol::state is complete at the point of destruction.
ModelEngine::~ModelEngine() = default;

// ─── on_frame ───────────────────────────────────────────────────────────────

void ModelEngine::on_frame(const CanFrame& frame) {
    store_.update(frame);

    sol::protected_function run = (*lua_)["run"];
    const sol::protected_function_result result = run();

    if (!result.valid()) {
        const sol::error err = result;
        std::println(stderr, "ModelEngine: run() error: {}", err.what());
        return;
    }

    if (!logger_) return;

    const std::string base = "model/" + script_name_ + "/";
    const sol::object ret  = result[0];

    if (ret.is<sol::table>()) {
        // Multiple named outputs: { brake_error = 42.0, avg = 7.5 }
        const sol::table t = ret.as<sol::table>();
        t.for_each([&](const sol::object& key, const sol::object& val) {
            if (!key.is<std::string>()) return;
            const auto v = to_double(val);
            if (v) logger_->log_scalar(base + key.as<std::string>(), *v);
        });
    } else {
        // Single numeric output
        const auto v = to_double(ret);
        if (v) logger_->log_scalar(base + "output", *v);
    }
}
