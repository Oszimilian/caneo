#pragma once

#include "frame/CanFrame.hpp"
#include "logger/Logger.hpp"
#include "proto/ProtoLogRegistry.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <utility>

using FrameTimestampMap =
    std::map<std::pair<std::string, uint32_t>, std::chrono::system_clock::time_point>;

inline void log_frame(const CanFrame& frame,
                      Logger* logger,
                      ProtoLogRegistry& proto_registry,
                      FrameTimestampMap& last_ts) {
    if (!logger) return;

    const std::string serialized = proto_registry.serialize(frame);
    const auto* proto_log        = proto_registry.get(frame.header().interface);
    const auto* desc             = proto_log ? proto_log->descriptor(frame.header().id) : nullptr;

    const auto key = std::make_pair(frame.header().interface, frame.header().id);
    std::optional<double> ratio_ms;
    if (const auto it = last_ts.find(key); it != last_ts.end())
        ratio_ms = std::chrono::duration<double, std::milli>(
            frame.timestamp() - it->second).count();
    last_ts[key] = frame.timestamp();

    logger->log(frame, desc, serialized, ratio_ms);
}
