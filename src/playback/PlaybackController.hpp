#pragma once

#include <algorithm>
#include <condition_variable>
#include <mutex>
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

private:
    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::vector<std::pair<std::string, bool>> states_;
    bool                    stopped_ = false;
};
