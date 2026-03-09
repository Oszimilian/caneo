#pragma once

#include "../frame/DataFrame.hpp"

#include <functional>
#include <memory>

class Socket {
public:
    using FrameCallback = std::function<void(std::unique_ptr<DataFrame>)>;

    virtual ~Socket() = default;

    virtual void start() = 0;
    virtual void stop() = 0;

    void onFrame(FrameCallback callback);

protected:
    FrameCallback callback_;
};
