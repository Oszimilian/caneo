#pragma once

#include "frame/CanFrame.hpp"

#include <google/protobuf/descriptor.h>

#include <string>

// Abstract base for all loggers (MCAP, UDP, gRPC, …).
class Logger {
public:
    virtual ~Logger() = default;

    // Log one CAN frame.
    // descriptor  – protobuf Descriptor for this message type (nullptr = skip)
    // serialized  – protobuf-binary payload of the message
    virtual void log(const CanFrame& frame,
                     const google::protobuf::Descriptor* descriptor,
                     const std::string& serialized) = 0;
};
