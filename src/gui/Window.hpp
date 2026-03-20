#pragma once

#include "frame/CanFrame.hpp"

class Window {
public:
    virtual ~Window() = default;

    // Called from asio thread when a new frame arrives.
    // Default is a no-op; override in windows that display live data.
    virtual void update(const CanFrame& frame) {}

    // Called from the main (render) thread each frame.
    virtual void render() = 0;
};
