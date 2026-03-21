#pragma once

#include "TraceWindow.hpp"
#include "Window.hpp"

#include <memory>
#include <vector>

class TraceContainerWindow : public Window {
public:
    void add(std::unique_ptr<TraceWindow> tw);

    void update(const CanFrame& frame) override;
    void render() override;

private:
    std::vector<std::unique_ptr<TraceWindow>> traces_;
};
