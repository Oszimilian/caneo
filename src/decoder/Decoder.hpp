#pragma once

#include "frame/CanFrame.hpp"

class Decoder {
public:
    virtual ~Decoder() = default;
    virtual void decode(CanFrame& frame) = 0;
};
