#pragma once

#include "GraphWindow.hpp"
#include "Window.hpp"
#include "frame/DataFrameSet.hpp"

#include <mutex>
#include <optional>
#include <string>

class TraceWindow : public Window {
public:
    TraceWindow(std::string interface_name, GraphWindow& graph_window);

    void update(const CanFrame& frame) override;
    void render() override;

    // Renders only the inner content (search bar + table + popups), no Begin/End.
    // Called by TraceContainerWindow inside a tab item.
    void render_content();

    const std::string& interface_name() const;

private:
    DataFrameSet  set_;
    std::mutex    mutex_;
    char          search_buf_[256] = {};
    GraphWindow*  graph_window_;

    struct PendingSignal {
        uint32_t    msg_id;
        std::string signal_name;
        double      y_min;
        double      y_max;
    };
    std::optional<PendingSignal> ctx_signal_;
    bool                         open_ctx_popup_ = false;
};
