#include "Socket.hpp"

void Socket::onFrame(Socket::FrameCallback callback) {
    callback_ = std::move(callback);
}
