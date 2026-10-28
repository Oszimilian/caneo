#include "GuardEngine.hpp"

#include <filesystem>

GuardEngine::GuardEngine(boost::asio::io_context& io, const SignalStore* store)
    : io_(io), store_(store)
{}

void GuardEngine::add(const GuardSpec& spec)
{
    // shared_ptr: asio handlers must be copyable
    auto g = std::make_shared<std::unique_ptr<Guard>>(std::make_unique<Guard>(spec, store_));
    boost::asio::post(io_, [this, g] {
        guards_.push_back(std::move(*g));
        rebuild_snapshot();
    });
}

void GuardEngine::add_all(const std::vector<GuardSpec>& specs)
{
    for (const auto& s : specs)
        add(s);
}

void GuardEngine::replace(std::size_t idx, const GuardSpec& spec)
{
    auto g = std::make_shared<std::unique_ptr<Guard>>(std::make_unique<Guard>(spec, store_));
    boost::asio::post(io_, [this, idx, g] {
        if (idx >= guards_.size()) return;
        guards_[idx] = std::move(*g);
        rebuild_snapshot();
    });
}

void GuardEngine::remove(std::size_t idx)
{
    boost::asio::post(io_, [this, idx] {
        if (idx >= guards_.size()) return;
        guards_.erase(guards_.begin() + static_cast<std::ptrdiff_t>(idx));
        rebuild_snapshot();
    });
}

void GuardEngine::set_enabled(std::size_t idx, bool enabled)
{
    boost::asio::post(io_, [this, idx, enabled] {
        if (idx >= guards_.size()) return;
        guards_[idx]->set_enabled(enabled);
        if (enabled && guards_[idx]->loaded())
            guards_[idx]->state()->log.set_error({});
        rebuild_snapshot();
    });
}

void GuardEngine::clear()
{
    boost::asio::post(io_, [this] {
        guards_.clear();
        rebuild_snapshot();
    });
}

int GuardEngine::reload_script(const std::string& script)
{
    std::error_code ec;
    const auto target = std::filesystem::weakly_canonical(expand_path(script), ec);
    int n = 0;
    for (const auto& info : snapshot()) {
        std::error_code ec2;
        if (std::filesystem::weakly_canonical(expand_path(info.spec.script), ec2) != target)
            continue;
        replace(info.idx, info.spec);
        ++n;
    }
    return n;
}

std::vector<GuardInfo> GuardEngine::snapshot() const
{
    std::lock_guard lock(snapshot_mutex_);
    return snapshot_;
}

void GuardEngine::on_frame(const CanFrame& frame)
{
    if (guards_.empty())
        return;
    const double t = std::chrono::duration<double>(frame.timestamp() - start_).count();
    bool changed = false;
    for (auto& g : guards_) {
        if (!g->matches(frame))
            continue;
        const bool was_enabled = g->enabled();
        g->evaluate(frame, t, output_fn_);
        changed |= was_enabled != g->enabled();   // disabled by an error
    }
    if (changed)
        rebuild_snapshot();
}

void GuardEngine::rebuild_snapshot()
{
    std::vector<GuardInfo> snap;
    snap.reserve(guards_.size());
    for (std::size_t i = 0; i < guards_.size(); ++i)
        snap.push_back({ i, guards_[i]->spec(), guards_[i]->loaded(), guards_[i]->state() });
    std::lock_guard lock(snapshot_mutex_);
    snapshot_ = std::move(snap);
}
