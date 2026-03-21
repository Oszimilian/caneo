#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct InterfaceConfig {
    std::string name;
    std::string dbc;           // empty if not set
    std::string actions_file;  // empty if not set
    std::string graphs_file;   // empty if not set
    uint32_t    baudrate = 0;  // 0 if not set
};

struct Config {
    bool                             virtual_can = false;
    std::optional<std::filesystem::path> log_file_path; // nullopt if not set
    std::vector<InterfaceConfig>     interfaces;
};

Config                load_config(const std::string& path);
std::optional<Config> try_load_default_config();
