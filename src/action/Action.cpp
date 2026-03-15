#include "Action.hpp"

#include <cmath>
#include <numbers>

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

void Action::pause() {
    paused_ = true;
    timer_.cancel();
}

void Action::resume() {
    paused_ = false;
    schedule();
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

// ─── SinPeriodicAction ──────────────────────────────────────────────────────

SinPeriodicAction::SinPeriodicAction(boost::asio::io_context&  io,
                                     std::string               interface,
                                     uint64_t                  msg_id,
                                     std::string               msg_name,
                                     SendFn                    send_fn,
                                     std::chrono::milliseconds interval,
                                     EncodeWithValueFn         encode_fn,
                                     double                    amplitude,
                                     std::chrono::milliseconds sin_period,
                                     double                    offset)
    : Action(io, std::move(interface), msg_id, std::move(msg_name), {}, std::move(send_fn))
    , interval_(interval)
    , encode_fn_(std::move(encode_fn))
    , amplitude_(amplitude)
    , sin_period_(sin_period)
    , offset_(offset)
    , resume_time_(std::chrono::steady_clock::now())
{}

double SinPeriodicAction::elapsed_seconds() const {
    return accumulated_.count() +
           std::chrono::duration<double>(std::chrono::steady_clock::now() - resume_time_).count();
}

void SinPeriodicAction::pause() {
    accumulated_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - resume_time_);
    Action::pause();
}

void SinPeriodicAction::resume() {
    resume_time_ = std::chrono::steady_clock::now();
    Action::resume();
}

void SinPeriodicAction::schedule() {
    timer_.expires_after(interval_);
    timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) return;
        const double omega = 2.0 * std::numbers::pi / (sin_period_.count() / 1000.0);
        payload_           = encode_fn_(amplitude_ * std::sin(omega * elapsed_seconds()) + offset_);
        fire();
        schedule();
    });
}
