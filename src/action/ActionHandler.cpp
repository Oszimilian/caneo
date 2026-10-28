#include "ActionHandler.hpp"

#include <chrono>

ActionHandler::ActionHandler(boost::asio::io_context& io, SendFn send_fn)
    : io_(io), send_fn_(std::move(send_fn)) {}

void ActionHandler::set_notify(std::function<void()> notify) {
    notify_ = std::move(notify);
}

void ActionHandler::add_action(std::unique_ptr<Action> action) {
    boost::asio::post(io_, [this, a = std::move(action)]() mutable {
        auto* ptr = a.get();
        actions_.push_back(std::move(a));
        actions_.back()->start(
            [this]      { on_action_fired(); },
            [this, ptr] { on_action_done(ptr); });
        rebuild_snapshot();
    });
}

void ActionHandler::add_action_paused(std::unique_ptr<Action> action) {
    boost::asio::post(io_, [this, a = std::move(action)]() mutable {
        auto* ptr = a.get();
        actions_.push_back(std::move(a));
        actions_.back()->start(
            [this]      { on_action_fired(); },
            [this, ptr] { on_action_done(ptr); });
        actions_.back()->pause();  // cancel timer immediately → starts paused
        rebuild_snapshot();
    });
}

void ActionHandler::remove_action(std::size_t idx) {
    boost::asio::post(io_, [this, idx] {
        if (idx >= actions_.size()) return;
        actions_[idx]->cancel();
        actions_.erase(actions_.begin() + static_cast<std::ptrdiff_t>(idx));
        rebuild_snapshot();
    });
}

std::vector<ActionInfo> ActionHandler::snapshot() const {
    std::lock_guard lock(snapshot_mutex_);
    return snapshot_;
}

void ActionHandler::toggle_pause(std::size_t idx) {
    boost::asio::post(io_, [this, idx] {
        if (idx >= actions_.size()) return;
        if (actions_[idx]->is_paused())
            actions_[idx]->resume();
        else
            actions_[idx]->pause();
        rebuild_snapshot();
    });
}

void ActionHandler::update_sin_param(std::size_t idx, int param, double value) {
    boost::asio::post(io_, [this, idx, param, value] {
        if (idx >= actions_.size()) return;
        auto* s = dynamic_cast<SinPeriodicAction*>(actions_[idx].get());
        if (!s) return;
        auto& spec = s->spec_ref();
        switch (param) {
            case 0: s->set_amplitude(value); spec.amplitude = value; break;
            case 1: s->set_sin_period(std::chrono::milliseconds(static_cast<long>(value)));
                    spec.sin_period_ms = static_cast<int>(value); break;
            case 2: s->set_offset(value); spec.offset = value; break;
        }
        rebuild_snapshot();
    });
}

void ActionHandler::update_payload(std::size_t idx, std::vector<uint8_t> payload) {
    boost::asio::post(io_, [this, idx, p = std::move(payload)]() mutable {
        if (idx < actions_.size())
            actions_[idx]->set_payload(std::move(p));
    });
}

void ActionHandler::set_all_paused(bool paused) {
    boost::asio::post(io_, [this, paused] {
        for (auto& a : actions_) {
            if (!a->is_periodic() || a->is_paused() == paused) continue;
            if (paused) a->pause();
            else        a->resume();
        }
        rebuild_snapshot();
    });
}

void ActionHandler::replace_action(std::size_t idx, std::unique_ptr<Action> action) {
    boost::asio::post(io_, [this, idx, a = std::move(action)]() mutable {
        if (idx >= actions_.size()) return;
        const bool was_paused = actions_[idx]->is_paused();
        actions_[idx]->cancel();
        auto* ptr = a.get();
        actions_[idx] = std::move(a);
        ptr->start(
            [this]      { on_action_fired(); },
            [this, ptr] { on_action_done(ptr); });
        if (was_paused) ptr->pause();
        rebuild_snapshot();
    });
}

void ActionHandler::clear() {
    boost::asio::post(io_, [this] {
        for (auto& a : actions_) a->cancel();
        actions_.clear();
        rebuild_snapshot();
    });
}

void ActionHandler::on_action_done(Action* ptr) {
    // already on asio thread
    std::erase_if(actions_, [ptr](const auto& a) { return a.get() == ptr; });
    rebuild_snapshot();
}

void ActionHandler::on_action_fired() {
    // called on asio thread after each send — update last_sent in snapshot
    rebuild_snapshot();
}

void ActionHandler::rebuild_snapshot() {
    std::vector<ActionInfo> snap;
    snap.reserve(actions_.size());
    for (std::size_t i = 0; i < actions_.size(); ++i) {
        snap.push_back(ActionInfo{
            .idx         = i,
            .msg_id      = actions_[i]->msg_id(),
            .msg_name    = actions_[i]->msg_name(),
            .type_name   = actions_[i]->type_name(),
            .interface   = actions_[i]->interface(),
            .is_periodic = actions_[i]->is_periodic(),
            .paused      = actions_[i]->is_paused(),
            .paused_at   = actions_[i]->paused_at(),
            .sin_amplitude = [&]{ auto* s = dynamic_cast<SinPeriodicAction*>(actions_[i].get()); return s ? s->amplitude()          : 0.0; }(),
            .sin_period_ms = [&]{ auto* s = dynamic_cast<SinPeriodicAction*>(actions_[i].get()); return s ? s->sin_period().count() : 0L;  }(),
            .sin_offset    = [&]{ auto* s = dynamic_cast<SinPeriodicAction*>(actions_[i].get()); return s ? s->offset()             : 0.0; }(),
            .period      = actions_[i]->period(),
            .last_sent   = actions_[i]->last_sent(),
            .ever_sent   = actions_[i]->ever_sent(),
            .spec        = actions_[i]->spec(),
            .log         = actions_[i]->log(),
        });
    }
    {
        std::lock_guard lock(snapshot_mutex_);
        snapshot_ = std::move(snap);
    }
    if (notify_) notify_();
}
