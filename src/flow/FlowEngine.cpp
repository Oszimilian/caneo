#include "FlowEngine.hpp"

FlowEngine::FlowEngine(boost::asio::io_context& io, const SignalStore* store, SendFn send_fn)
    : io_(io), store_(store), send_fn_(std::move(send_fn))
{}

// shared_ptr: asio handlers must be copyable
std::shared_ptr<std::unique_ptr<Flow>> FlowEngine::make(const FlowSpec& spec)
{
    return std::make_shared<std::unique_ptr<Flow>>(
        std::make_unique<Flow>(io_, spec, store_, send_fn_, output_fn_, model_fn_));
}

void FlowEngine::add(const FlowSpec& spec)
{
    auto f = make(spec);
    boost::asio::post(io_, [this, f] {
        flows_.push_back(std::move(*f));
        if (flows_.back()->spec().enabled) flows_.back()->start();
        rebuild_snapshot();
    });
}

void FlowEngine::add_all(const std::vector<FlowSpec>& specs)
{
    for (const auto& s : specs)
        add(s);
}

void FlowEngine::replace(std::size_t idx, const FlowSpec& spec)
{
    auto f = make(spec);
    boost::asio::post(io_, [this, idx, f] {
        if (idx >= flows_.size()) return;
        flows_[idx]->stop();
        flows_[idx] = std::move(*f);
        if (flows_[idx]->spec().enabled) flows_[idx]->start();
        rebuild_snapshot();
    });
}

void FlowEngine::remove(std::size_t idx)
{
    boost::asio::post(io_, [this, idx] {
        if (idx >= flows_.size()) return;
        flows_[idx]->stop();
        flows_.erase(flows_.begin() + static_cast<std::ptrdiff_t>(idx));
        rebuild_snapshot();
    });
}

void FlowEngine::set_enabled(std::size_t idx, bool enabled)
{
    // Re-creates the flow (fresh Lua state, error cleared). Called from the
    // GUI thread: the DBC lookup in model_fn_ is not thread-safe.
    const auto snap = snapshot();
    if (idx >= snap.size()) return;
    FlowSpec spec = snap[idx].spec;
    spec.enabled  = enabled;
    replace(idx, spec);
}

void FlowEngine::trigger(std::size_t idx, int node_id)
{
    boost::asio::post(io_, [this, idx, node_id] {
        if (idx < flows_.size()) flows_[idx]->trigger(node_id);
    });
}

void FlowEngine::set_control(std::size_t idx, int node_id, double value)
{
    boost::asio::post(io_, [this, idx, node_id, value] {
        if (idx >= flows_.size()) return;
        flows_[idx]->set_control(node_id, value);
        rebuild_snapshot();   // the snapshot's spec carries the slider value (saved with the config)
    });
}

void FlowEngine::clear()
{
    boost::asio::post(io_, [this] {
        for (auto& f : flows_) f->stop();
        flows_.clear();
        rebuild_snapshot();
    });
}

std::vector<FlowInfo> FlowEngine::snapshot() const
{
    std::lock_guard lock(snapshot_mutex_);
    return snapshot_;
}

void FlowEngine::rebuild_snapshot()
{
    std::vector<FlowInfo> snap;
    snap.reserve(flows_.size());
    for (std::size_t i = 0; i < flows_.size(); ++i)
        snap.push_back({ i, flows_[i]->spec(), flows_[i]->loaded(), flows_[i]->running(), flows_[i]->state() });
    {
        std::lock_guard lock(snapshot_mutex_);
        snapshot_ = std::move(snap);
    }
    if (notify_) notify_();
}
