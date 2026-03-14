#pragma once

#include "frame/CanFrame.hpp"

#include <google/protobuf/descriptor.h>

#include <optional>
#include <string>

// Abstract base for all loggers (MCAP, UDP, gRPC, …).
class Logger {
public:
    virtual ~Logger() = default;

    // Log one CAN frame.
    // descriptor       – protobuf Descriptor for this message type (nullptr = skip)
    // serialized       – protobuf-binary payload of the message
    // update_ratio_ms  – time since last frame with this CAN ID (ms), absent on first frame
    virtual void log(const CanFrame& frame,
                     const google::protobuf::Descriptor* descriptor,
                     const std::string& serialized,
                     std::optional<double> update_ratio_ms = std::nullopt) = 0;

    // Log a single named scalar value (e.g. model output).
    virtual void log_scalar(const std::string& topic, double value) = 0;
};
