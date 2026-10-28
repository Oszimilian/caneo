#pragma once

#include "config/Config.hpp"

#include <string>

// Minimal interface for starting/stopping a log and querying its state.
// Implemented by LogController (local) and RemoteLogControl (gRPC).
class ILogControl {
public:
    virtual ~ILogControl() = default;
    virtual void        start()           = 0;
    virtual void        stop()            = 0;
    virtual bool        is_logging()      const = 0;
    virtual std::string current_path()    const = 0;
    virtual double      elapsed_seconds() const = 0;  // 0 if not logging

    // Start logging into a specific file. Implementations that cannot choose
    // the path (remote logging) ignore it, see supports_path().
    virtual void start(const std::string& path) { (void)path; start(); }
    virtual bool supports_path() const { return false; }

    // File of the most recently finished log (empty if none). Local only.
    virtual std::string last_path() const { return {}; }

    // What to log (raw/update_ratio/...). Remote logging ignores it.
    virtual void set_options(const LogOptions& options) { (void)options; }
};
