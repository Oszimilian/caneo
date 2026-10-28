#pragma once

#include "config/Config.hpp"

#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Thread-safe text log + error state of one action (e.g. Lua print() output
// and script errors). Written on the asio thread, read by the GUI.
class ActionLog {
public:
    static constexpr std::size_t MAX_LINES = 500;

    void append(std::string line);
    void set_error(std::string error);
    void clear();

    std::vector<std::string> lines() const;
    std::string              error() const;

private:
    mutable std::mutex      mutex_;
    std::deque<std::string> lines_;
    std::string             error_;
};

using SendFn = std::function<void(const std::string& iface, uint64_t id, const std::vector<uint8_t>& data)>;

class Action {
public:
    Action(boost::asio::io_context& io,
           std::string              interface,
           uint64_t                 msg_id,
           std::string              msg_name,
           std::vector<uint8_t>     payload,
           SendFn                   send_fn);

    virtual ~Action() = default;

    virtual bool                      is_periodic() const = 0;
    virtual std::string               type_name()   const = 0;
    virtual std::chrono::milliseconds period()      const = 0;

    const std::string&                    interface() const { return interface_; }
    uint64_t                              msg_id()    const { return msg_id_; }
    const std::string&                    msg_name()  const { return msg_name_; }
    std::chrono::steady_clock::time_point last_sent() const { return last_sent_; }
    bool                                  ever_sent() const { return ever_sent_; }

    void start(std::function<void()> on_fired, std::function<void()> on_done);
    void cancel();
    virtual void pause();   // cancels timer, sets paused flag
    virtual void resume();  // clears paused flag, reschedules
    bool is_paused() const { return paused_; }
    // When the action was paused (valid while is_paused()).
    std::chrono::steady_clock::time_point paused_at() const { return paused_at_; }
    void set_payload(std::vector<uint8_t> payload) { payload_ = std::move(payload); }

    // Description this action was created from (empty type = ad-hoc action
    // that cannot be saved). Set by ActionFactory.
    const ActionSpec&           spec() const { return spec_; }
    void                        set_spec(ActionSpec spec) { spec_ = std::move(spec); }
    ActionSpec&                 spec_ref() { return spec_; }
    std::shared_ptr<ActionLog>  log() const { return log_; }

protected:
    void fire();
    virtual void schedule() = 0;

    boost::asio::steady_timer             timer_;
    std::string                           interface_;
    uint64_t                              msg_id_;
    std::string                           msg_name_;
    std::vector<uint8_t>                  payload_;
    SendFn                                send_fn_;
    std::function<void()>                 on_fired_;
    std::function<void()>                 on_done_;
    std::chrono::steady_clock::time_point last_sent_{};
    bool                                  ever_sent_ = false;
    bool                                  paused_    = false;
    std::chrono::steady_clock::time_point paused_at_{};
    ActionSpec                            spec_;
    std::shared_ptr<ActionLog>            log_ = std::make_shared<ActionLog>();
};

class SingleAction : public Action {
public:
    using Action::Action;
    bool                      is_periodic() const override { return false; }
    std::string               type_name()   const override { return "Single"; }
    std::chrono::milliseconds period()      const override { return {}; }
protected:
    void schedule() override;
};

class PeriodicAction : public Action {
public:
    PeriodicAction(boost::asio::io_context&  io,
                   std::string               interface,
                   uint64_t                  msg_id,
                   std::string               msg_name,
                   std::vector<uint8_t>      payload,
                   SendFn                    send_fn,
                   std::chrono::milliseconds period);

    bool                      is_periodic() const override { return true; }
    std::string               type_name()   const override { return "Periodic"; }
    std::chrono::milliseconds period()      const override { return period_; }
protected:
    void schedule() override;
private:
    std::chrono::milliseconds period_;
};

class SinPeriodicAction : public Action {
public:
    using EncodeWithValueFn = std::function<std::vector<uint8_t>(double)>;

    SinPeriodicAction(boost::asio::io_context&  io,
                      std::string               interface,
                      uint64_t                  msg_id,
                      std::string               msg_name,
                      SendFn                    send_fn,
                      std::chrono::milliseconds interval,
                      EncodeWithValueFn         encode_fn,
                      double                    amplitude,
                      std::chrono::milliseconds sin_period,
                      double                    offset);

    bool                      is_periodic() const override { return true; }
    std::string               type_name()   const override { return "Sin"; }
    std::chrono::milliseconds period()      const override { return interval_; }

    void pause() override;   // accumulates elapsed time before pausing
    void resume() override;  // records resume timestamp

    // Getters
    double                    amplitude()  const { return amplitude_; }
    std::chrono::milliseconds sin_period() const { return sin_period_; }
    double                    offset()     const { return offset_; }

    // Setters (call on asio thread via ActionHandler::update_sin_param)
    void set_amplitude(double v)                      { amplitude_  = v; }
    void set_sin_period(std::chrono::milliseconds ms) { sin_period_ = ms; }
    void set_offset(double v)                         { offset_     = v; }

protected:
    void schedule() override;
private:
    double elapsed_seconds() const;

    std::chrono::milliseconds             interval_;
    EncodeWithValueFn                     encode_fn_;
    double                                amplitude_;
    std::chrono::milliseconds             sin_period_;
    double                                offset_;
    std::chrono::steady_clock::time_point resume_time_;
    std::chrono::duration<double>         accumulated_{};
};
