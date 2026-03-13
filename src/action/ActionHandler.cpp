#include "ActionHandler.hpp"

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
            .interface   = actions_[i]->interface(),
            .is_periodic = actions_[i]->is_periodic(),
            .period      = actions_[i]->period(),
            .last_sent   = actions_[i]->last_sent(),
            .ever_sent   = actions_[i]->ever_sent(),
        });
    }
    {
        std::lock_guard lock(snapshot_mutex_);
        snapshot_ = std::move(snap);
    }
    if (notify_) notify_();
}
