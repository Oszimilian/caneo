#pragma once

#include "Config.hpp"
#include <string>

struct AppConfig {
    bool        tui_mode      = false;
    bool        gui_mode      = false;
    bool        log_mode      = false;
    bool        debug_mode    = false;
    std::string model_path;
    std::string playback_path;
    Config      config;

    // Parses gflags + positional interface args + loads Config.
    // Throws std::runtime_error on invalid input.
    static AppConfig parse(int& argc, char* argv[]);

    // Returns a timestamped .mcap path under config.log_file_path (or cwd).
    // Creates the directory if it does not exist.
    std::string make_log_path() const;
};
