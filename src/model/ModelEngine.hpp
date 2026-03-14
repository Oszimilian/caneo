#pragma once

#include "SignalStore.hpp"
#include "logger/Logger.hpp"

#include <filesystem>
#include <memory>
#include <string>

// Forward-declare sol::state to keep sol.hpp out of this header.
namespace sol { class state; }

// Runs a user-supplied Lua script on every CAN frame.
//
// The script must define a global function run() that returns either:
//   - a number  → logged as model/<script_name>/output
//   - a table   → each {key, number} pair logged as model/<script_name>/<key>
//   - nil       → output is silently ignored
//
// Lua API exposed to the script:
//   get_signal(iface, msg, signal) → number (0 if not yet received)
//
// All methods must be called exclusively from the asio thread.
class ModelEngine {
public:
    // script_path  – path to the .lua file; stem is used as the script name
    // store        – signal store, must outlive ModelEngine
    // logger       – may be nullptr (model runs but outputs are discarded)
    ModelEngine(const std::filesystem::path& script_path,
                SignalStore& store,
                Logger* logger);
    ~ModelEngine();

    ModelEngine(const ModelEngine&)            = delete;
    ModelEngine& operator=(const ModelEngine&) = delete;

    // Feed a decoded frame into the signal store, then call run().
    void on_frame(const CanFrame& frame);

    const std::string& script_name() const { return script_name_; }

private:
    void dispatch_outputs(const sol::state& lua, int n_results);

    std::unique_ptr<sol::state> lua_;
    SignalStore&                store_;
    Logger*                     logger_;
    std::string                 script_name_;
};
