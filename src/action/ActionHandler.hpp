#pragma once

#include "Action.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct ActionInfo {
    std::size_t                           idx;
    uint64_t                              msg_id;
    std::string                           msg_name;
    std::string                           type_name;
    std::string                           interface;
    bool                                  is_periodic;
    bool                                  paused;
    // Sin-specific (only valid when type_name == "Sin")
    double                                sin_amplitude = 0;
    long                                  sin_period_ms = 0;
    double                                sin_offset    = 0;
    std::chrono::milliseconds             period;
    std::chrono::steady_clock::time_point last_sent;
    bool                                  ever_sent;
};

class ActionHandler {
public:
    ActionHandler(boost::asio::io_context& io, SendFn send_fn);

    // Thread-safe: callable from any thread
    void add_action(std::unique_ptr<Action> action);
    void remove_action(std::size_t idx);
    void toggle_pause(std::size_t idx);
    void update_sin_param(std::size_t idx, int param, double value); // param: 0=amplitude 1=period 2=offset
    void update_payload(std::size_t idx, std::vector<uint8_t> payload);

    // Accessors for constructing Action objects
    boost::asio::io_context& io_ref()      { return io_; }
    const SendFn&            send_fn_ref() { return send_fn_; }

    // Returns a snapshot of current actions — thread-safe
    std::vector<ActionInfo> snapshot() const;

    // Called by TuiDataFrameSet to get notified when actions change
    void set_notify(std::function<void()> notify);

private:
    void on_action_done(Action* ptr);   // posted to asio thread
    void on_action_fired();             // called on asio thread
    void rebuild_snapshot();            // called on asio thread

    boost::asio::io_context& io_;
    SendFn                   send_fn_;
    std::function<void()>    notify_;

    // Only accessed on the asio thread
    std::vector<std::unique_ptr<Action>> actions_;

    // Protected by snapshot_mutex_, read from ftxui thread
    mutable std::mutex      snapshot_mutex_;
    std::vector<ActionInfo> snapshot_;
};
