#pragma once

#include "Guard.hpp"

#include <boost/asio.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class SignalStore;

struct GuardInfo {
    std::size_t                 idx;
    GuardSpec                   spec;     // enabled reflects the current state
    bool                        loaded;   // script loaded without error
    std::shared_ptr<GuardState> state;    // live values + log
};

// Owns all guards and feeds them the received frames.
//
// Like ActionHandler: list mutations are posted to the io_context, so they
// may be called from any thread; on_frame() runs on the asio thread.
// Guards (and their Lua scripts) are constructed on the calling thread.
class GuardEngine {
public:
    GuardEngine(boost::asio::io_context& io, const SignalStore* store);

    // Called for every output of every evaluation (asio thread), e.g. to log
    // it or to feed a graph. Set before frames arrive.
    void set_output_fn(Guard::OutputFn fn) { output_fn_ = std::move(fn); }

    void add(const GuardSpec& spec);
    void add_all(const std::vector<GuardSpec>& specs);
    void replace(std::size_t idx, const GuardSpec& spec);   // also reloads the script
    void remove(std::size_t idx);
    void set_enabled(std::size_t idx, bool enabled);
    void clear();

    // Re-creates every guard whose script is `script` (after editing it).
    // Returns the number of guards reloaded.
    int reload_script(const std::string& script);

    std::vector<GuardInfo> snapshot() const;

    // asio thread only
    void on_frame(const CanFrame& frame);

private:
    void rebuild_snapshot();   // asio thread

    boost::asio::io_context&              io_;
    const SignalStore*                    store_;
    Guard::OutputFn                       output_fn_;
    std::chrono::system_clock::time_point start_ = std::chrono::system_clock::now();

    std::vector<std::unique_ptr<Guard>>   guards_;   // asio thread only

    mutable std::mutex     snapshot_mutex_;
    std::vector<GuardInfo> snapshot_;
};
