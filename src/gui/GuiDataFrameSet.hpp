#pragma once

#include "frame/CanFrame.hpp"

class GuiDataFrameSet {
public:
    virtual ~GuiDataFrameSet() = default;
    virtual void update(const CanFrame& frame) = 0;
    virtual void run() = 0;
};
