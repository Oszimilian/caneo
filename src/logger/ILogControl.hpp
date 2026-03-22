#pragma once

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
};
