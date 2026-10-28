#pragma once

#include "TraceWindow.hpp"
#include "Window.hpp"

#include <memory>
#include <vector>

// "Trace" window: one tab per interface plus a "Guards" tab.
class TraceContainerWindow : public Window {
public:
    void add(std::unique_ptr<TraceWindow> tw);
    void set_add_guard_callback(TraceWindow::AddGuardFn cb);

    const char* title() const override { return "Trace"; }

    void update(const CanFrame& frame) override;
    void render() override;

private:
    std::vector<std::unique_ptr<TraceWindow>> traces_;
};
