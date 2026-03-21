#pragma once

#include "GraphWindow.hpp"
#include "Window.hpp"
#include "action/ActionHandler.hpp"
#include "config/Config.hpp"
#include "frame/CanFrame.hpp"

#include <atomic>
#include <memory>
#include <vector>

class ModelWindow;

class Gui {
public:
    Gui(const std::vector<InterfaceConfig>& iface_configs,
        ActionHandler& action_handler);

    // Thread-safe: called from asio thread, delegates to all windows.
    void update(const CanFrame& frame);

    // Blocks until window is closed; call from main thread.
    void run();

    // Thread-safe: signals the render loop to exit cleanly.
    void stop();

    // Creates a ModelWindow, adds it to the render loop, returns the raw pointer.
    // Must be called before run(). The window is owned by Gui.
    ModelWindow* add_model_window();

    // Returns the shared GraphWindow. Valid for the lifetime of this Gui.
    GraphWindow* graph_window() { return graph_window_; }

private:
    GraphWindow*                         graph_window_ = nullptr;
    std::vector<std::unique_ptr<Window>> windows_;
    std::atomic<bool>                    stop_requested_{false};
};
