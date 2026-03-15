#pragma once

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Thread-safe per-interface playback state.
// All interfaces start in "paused" state.
class PlaybackController {
public:
    explicit PlaybackController(const std::vector<std::string>& interfaces) {
        for (const auto& iface : interfaces)
            states_.emplace_back(iface, false);
    }

    bool is_running(const std::string& iface) const {
        std::lock_guard lock(mutex_);
        for (const auto& [name, running] : states_)
            if (name == iface) return running;
        return false;
    }

    bool any_running() const {
        std::lock_guard lock(mutex_);
        return std::any_of(states_.begin(), states_.end(),
                           [](const auto& p) { return p.second; });
    }

    void toggle(const std::string& iface) {
        {
            std::lock_guard lock(mutex_);
            for (auto& [name, running] : states_)
                if (name == iface) { running = !running; break; }
        }
        cv_.notify_all();
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

    // Like sleep_until, but wakes early if stop() is called.
    // Returns true if stopped, false if the time point was reached normally.
    bool sleep_until(std::chrono::steady_clock::time_point tp) {
        std::unique_lock lock(mutex_);
        cv_.wait_until(lock, tp, [this] { return stopped_; });
        return stopped_;
    }

    bool is_stopped() const {
        std::lock_guard lock(mutex_);
        return stopped_;
    }

    // Blocks the calling thread until at least one interface is running or stop() is called.
    void wait_for_any_running() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] {
            return stopped_ || std::any_of(states_.begin(), states_.end(),
                                           [](const auto& p) { return p.second; });
        });
    }

    std::vector<std::pair<std::string, bool>> snapshot() const {
        std::lock_guard lock(mutex_);
        return states_;
    }

    // ── Per-interface message filter ──────────────────────────────────────

    void set_messages(const std::string& iface, const std::vector<std::string>& names) {
        std::lock_guard lock(mutex_);
        auto& vec = messages_[iface];
        vec.clear();
        for (const auto& n : names)
            vec.emplace_back(n, true);
    }

    // idx == 0  → toggle all (any enabled → disable all; all disabled → enable all)
    // idx >= 1  → toggle that message (1-based)
    void toggle_message(const std::string& iface, int idx) {
        std::lock_guard lock(mutex_);
        auto it = messages_.find(iface);
        if (it == messages_.end()) return;
        auto& vec = it->second;
        if (idx == 0) {
            const bool any = std::any_of(vec.begin(), vec.end(),
                                         [](const auto& p) { return p.second; });
            for (auto& [name, enabled] : vec) enabled = !any;
        } else {
            const int i = idx - 1;
            if (i < static_cast<int>(vec.size()))
                vec[i].second = !vec[i].second;
        }
    }

    bool is_message_enabled(const std::string& iface, const std::string& msg_name) const {
        std::lock_guard lock(mutex_);
        auto it = messages_.find(iface);
        if (it == messages_.end()) return true;
        for (const auto& [name, enabled] : it->second)
            if (name == msg_name) return enabled;
        return true;
    }

    // Returns { msg_name, enabled } list for one interface.
    std::vector<std::pair<std::string, bool>> message_snapshot(const std::string& iface) const {
        std::lock_guard lock(mutex_);
        auto it = messages_.find(iface);
        if (it == messages_.end()) return {};
        return it->second;
    }

private:
    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::vector<std::pair<std::string, bool>> states_;
    bool                    stopped_ = false;
    std::map<std::string, std::vector<std::pair<std::string, bool>>> messages_;
};
