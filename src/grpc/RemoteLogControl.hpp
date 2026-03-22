#pragma once

#include "logger/ILogControl.hpp"
#include "can_stream.grpc.pb.h"

#include <chrono>
#include <memory>
#include <string>

// Controls the server-side logger remotely via gRPC (StartLog / StopLog / GetLogStatus).
// Called from the GUI (render) thread only.
class RemoteLogControl : public ILogControl {
public:
    explicit RemoteLogControl(const std::string& server_address);

    void        start()           override;
    void        stop()            override;
    bool        is_logging()      const override { return logging_; }
    std::string current_path()    const override { return path_; }
    double      elapsed_seconds() const override;

private:
    void refresh_status();

    std::unique_ptr<caneo::CanStream::Stub>       stub_;
    bool                                          logging_ = false;
    std::string                                   path_;
    std::chrono::steady_clock::time_point         log_start_time_;
};
