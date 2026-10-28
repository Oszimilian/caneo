#pragma once

#include "GraphWindow.hpp"
#include "Window.hpp"
#include "frame/DataFrameSet.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <string>

class TraceWindow : public Window {
public:
    TraceWindow(std::string interface_name, GraphWindow& graph_window);

    const char* title() const override { return title_.c_str(); }

    void update(const CanFrame& frame) override;
    void render() override;

    // Renders only the inner content (search bar + table + popups), no Begin/End.
    // Called by TraceContainerWindow inside a tab item.
    void render_content();

    const std::string& interface_name() const;

    // Called by the "Add guard..." context entry with (interface, msg, signal).
    using AddGuardFn = std::function<void(const std::string&, const std::string&, const std::string&)>;
    void set_add_guard_callback(AddGuardFn cb) { add_guard_cb_ = std::move(cb); }

private:
    DataFrameSet  set_;
    std::mutex    mutex_;
    char          search_buf_[256] = {};
    GraphWindow*  graph_window_;
    std::string   title_;
    AddGuardFn    add_guard_cb_;

    struct PendingSignal {
        uint32_t    msg_id;
        std::string msg_name;
        std::string signal_name;
        double      y_min;
        double      y_max;
    };
    std::optional<PendingSignal> ctx_signal_;
    bool                         open_ctx_popup_ = false;
};
