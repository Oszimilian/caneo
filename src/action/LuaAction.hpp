#pragma once

#include "Action.hpp"
#include "send/SendModel.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

class SignalStore;
namespace sol { class state; }

// Periodic action driven by a Lua script.
//
// The script defines tick(t), which is called every period with t = seconds
// the action has been running (pauses excluded). Optional: init(), called once
// after loading. API available to the script:
//
//   send(msg, {SIG = value, ...} [, iface])  encode via DBC and send; signals
//                                            not given keep their last value
//   send_raw(id, {b0, b1, ...} [, iface])    send raw bytes
//   get_signal(iface, msg, signal)           last received value (0 if none)
//   print(...)                               output to the action log
//   interface, period_ms                     globals with the action settings
//
// A runtime error pauses the action and is shown in the GUI; resuming retries.
// All methods run on the asio thread except the constructor.
class LuaAction : public Action {
public:
    using ModelMap = std::map<std::string, std::shared_ptr<const SendModel>>;

    LuaAction(boost::asio::io_context&     io,
              std::string                  interface,
              SendFn                       send_fn,
              std::chrono::milliseconds    period,
              const std::filesystem::path& script_path,
              ModelMap                     models,
              const SignalStore*           store);
    ~LuaAction() override;

    bool                      is_periodic() const override { return true; }
    std::string               type_name()   const override { return "Lua"; }
    std::chrono::milliseconds period()      const override { return period_; }

    void pause() override;
    void resume() override;

protected:
    void schedule() override;

private:
    bool   load(const std::filesystem::path& script_path);
    void   tick();
    void   fail(const std::string& what);
    double elapsed_seconds() const;
    void   register_api();

    std::unique_ptr<sol::state>  lua_;
    bool                         loaded_ = false;
    std::chrono::milliseconds    period_;
    ModelMap                     models_;
    const SignalStore*           store_;

    // Last physical values per (interface, message index), so a send() only
    // has to name the signals that change.
    std::map<std::pair<std::string, int>, std::vector<double>> values_;

    std::chrono::steady_clock::time_point resume_time_;
    std::chrono::duration<double>         accumulated_{};
};
