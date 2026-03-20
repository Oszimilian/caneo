#pragma once

#include "Window.hpp"
#include "action/ActionHandler.hpp"
#include "config/Config.hpp"
#include "frame/CanFrame.hpp"

#include <memory>
#include <vector>

class Gui {
public:
    Gui(const std::vector<InterfaceConfig>& iface_configs,
        ActionHandler& action_handler);

    // Thread-safe: called from asio thread, delegates to all windows.
    void update(const CanFrame& frame);

    // Blocks until window is closed; call from main thread.
    void run();

private:
    std::vector<std::unique_ptr<Window>> windows_;
};
