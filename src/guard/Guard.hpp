#pragma once

#include "action/Action.hpp"   // ActionLog
#include "config/Config.hpp"
#include "frame/CanFrame.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class SignalStore;
namespace sol { class state; }

// Live values of a guard; written on the asio thread, read by the GUI.
class GuardState {
public:
    struct Values {
        std::optional<double>                       input;
        std::vector<std::pair<std::string, double>> outputs;   // first-seen order
        uint64_t                                    count = 0; // evaluations
        std::chrono::steady_clock::time_point       last_run{};
    };

    void   set(double input, const std::vector<std::pair<std::string, double>>& outputs);
    Values values() const;
    void   reset();

    ActionLog log;   // print() output + error

private:
    mutable std::mutex mutex_;
    Values             values_;
};

// Watches one signal (spec.interface / spec.msg / spec.signal). For every
// received frame of that message, check(value, t) of the Lua script runs;
// its return value - a number, a boolean or a table {name = value} - becomes
// the guard's outputs.
//
// Lua API: get_signal(iface, msg, signal), print(...), globals interface,
// msg, signal. Optional init() runs once after loading.
//
// Constructed on any thread; evaluate() only on the asio thread.
class Guard {
public:
    using OutputFn = std::function<void(const std::string& guard, const std::string& output, double value)>;

    Guard(GuardSpec spec, const SignalStore* store);
    ~Guard();

    Guard(const Guard&)            = delete;
    Guard& operator=(const Guard&) = delete;

    const GuardSpec&                   spec() const  { return spec_; }
    const std::string&                 name() const  { return spec_.name; }
    std::shared_ptr<GuardState>        state() const { return state_; }
    bool                               loaded() const { return loaded_; }

    bool enabled() const      { return spec_.enabled; }
    void set_enabled(bool on) { spec_.enabled = on; }

    // True if frame is the guarded message on the guarded interface.
    bool matches(const CanFrame& frame) const;

    // Runs check(); t = seconds since the engine started. Errors are stored
    // in state()->log and disable the guard until it is re-enabled.
    void evaluate(const CanFrame& frame, double t, const OutputFn& out);

    // Script template for new guard scripts.
    static const char* TEMPLATE;

private:
    bool load();

    GuardSpec                   spec_;
    const SignalStore*          store_;
    std::unique_ptr<sol::state> lua_;
    bool                        loaded_ = false;
    std::shared_ptr<GuardState> state_ = std::make_shared<GuardState>();
    std::vector<std::pair<std::string, double>> outputs_;   // reused buffer
};
