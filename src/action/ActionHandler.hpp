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
    std::string                           interface;
    bool                                  is_periodic;
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
