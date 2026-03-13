#include "Action.hpp"

Action::Action(boost::asio::io_context& io,
               std::string              interface,
               uint64_t                 msg_id,
               std::string              msg_name,
               std::vector<uint8_t>     payload,
               SendFn                   send_fn)
    : timer_(io)
    , interface_(std::move(interface))
    , msg_id_(msg_id)
    , msg_name_(std::move(msg_name))
    , payload_(std::move(payload))
    , send_fn_(std::move(send_fn))
{}

void Action::start(std::function<void()> on_fired, std::function<void()> on_done) {
    on_fired_ = std::move(on_fired);
    on_done_  = std::move(on_done);
    schedule();
}

void Action::cancel() {
    timer_.cancel();
}

void Action::fire() {
    send_fn_(interface_, msg_id_, payload_);
    last_sent_ = std::chrono::steady_clock::now();
    ever_sent_ = true;
    if (on_fired_) on_fired_();
}

// ─── SingleAction ──────────────────────────────────────────────────────────

void SingleAction::schedule() {
    timer_.expires_after(std::chrono::milliseconds(0));
    timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) return;
        fire();
        if (on_done_) on_done_();
    });
}

// ─── PeriodicAction ────────────────────────────────────────────────────────

PeriodicAction::PeriodicAction(boost::asio::io_context&  io,
                               std::string               interface,
                               uint64_t                  msg_id,
                               std::string               msg_name,
                               std::vector<uint8_t>      payload,
                               SendFn                    send_fn,
                               std::chrono::milliseconds period)
    : Action(io, std::move(interface), msg_id, std::move(msg_name),
             std::move(payload), std::move(send_fn))
    , period_(period)
{}

void PeriodicAction::schedule() {
    timer_.expires_after(period_);
    timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) return;
        fire();
        schedule();
    });
}
