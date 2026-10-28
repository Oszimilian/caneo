#pragma once

#include "frame/CanFrame.hpp"

class Window {
public:
    virtual ~Window() = default;

    // Called from asio thread when a new frame arrives.
    // Default is a no-op; override in windows that display live data.
    virtual void update(const CanFrame& frame) {}

    // Called from the main (render) thread each frame while visible.
    virtual void render() = 0;

    // ImGui window name; also identifies the window in the dock layout.
    virtual const char* title() const = 0;

    // Toggled by the window's close button and the View menu.
    bool visible = true;
};
