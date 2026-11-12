#pragma once

#include "Config.hpp"
#include <string>
#include <vector>

struct AppConfig {
    bool        tui_mode      = false;
    bool        gui_mode      = false;
    bool        headless_mode = false;   // no UI: print frames / log (the former default)
    bool        config_found  = false;   // a config file was loaded or interfaces were given on the command line
    bool        log_mode      = false;
    bool        debug_mode    = false;
    bool        grpc_server   = false;
    std::string grpc_client;       // empty = not a client; "host:port" = connect here
    uint16_t    grpc_port     = 50051;
    std::string model_path;
    std::string playback_path;
    std::string config_path = "caneo.yaml"; // where the GUI saves the config
    Config      config;

    // Parses gflags + positional interface args + loads Config.
    // Throws std::runtime_error on invalid input.
    static AppConfig parse(int& argc, char* argv[]);

    // Returns a timestamped .mcap path under config.log_file_path (or cwd).
    // Creates the directory if it does not exist.
    std::string make_log_path() const;

    // "caneo_YYYYmmdd_HHMMSS.mcap" for the current local time.
    static std::string log_file_name();

    // Command line to re-exec caneo with the same mode flags and the config
    // file at config_path (used by the GUI's "Save & Restart").
    std::vector<std::string> restart_args() const;
};
