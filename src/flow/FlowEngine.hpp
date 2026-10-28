#pragma once

#include "Flow.hpp"

#include <boost/asio.hpp>
#include <memory>
#include <mutex>
#include <vector>

struct FlowInfo {
    std::size_t                idx;
    FlowSpec                   spec;      // enabled reflects the current state
    bool                       loaded;    // built without error
    bool                       running;
    std::shared_ptr<FlowState> state;     // live values + log
};

// Owns all flows. Like GuardEngine/ActionHandler, list mutations are posted
// to the io_context, so they may be called from any thread. Flows are built
// (DBC lookup, Lua) on the calling thread and run on the asio thread.
class FlowEngine {
public:
    FlowEngine(boost::asio::io_context& io, const SignalStore* store, SendFn send_fn);

    void set_output_fn(Flow::OutputFn fn) { output_fn_ = std::move(fn); }
    void set_model_fn(Flow::ModelFn fn)   { model_fn_ = std::move(fn); }
    void set_notify(std::function<void()> fn) { notify_ = std::move(fn); }

    void add(const FlowSpec& spec);
    void add_all(const std::vector<FlowSpec>& specs);
    void replace(std::size_t idx, const FlowSpec& spec);
    void remove(std::size_t idx);
    void set_enabled(std::size_t idx, bool enabled);
    void clear();
    // Sends the message of a can_out block now (the flow has to be running).
    void trigger(std::size_t idx, int node_id);

    std::vector<FlowInfo> snapshot() const;

private:
    std::shared_ptr<std::unique_ptr<Flow>> make(const FlowSpec& spec);
    void rebuild_snapshot();   // asio thread

    boost::asio::io_context& io_;
    const SignalStore*       store_;
    SendFn                   send_fn_;
    Flow::OutputFn           output_fn_;
    Flow::ModelFn            model_fn_;
    std::function<void()>    notify_;

    std::vector<std::unique_ptr<Flow>> flows_;   // asio thread only

    mutable std::mutex    snapshot_mutex_;
    std::vector<FlowInfo> snapshot_;
};
