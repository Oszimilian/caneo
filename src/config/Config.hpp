#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct InterfaceConfig {
    std::string name;
    std::string dbc;        // empty if not set
    uint32_t    baudrate = 0; // 0 if not set
};

struct Config {
    bool                        virtual_can = false;
    std::vector<InterfaceConfig> interfaces;
};

Config                load_config(const std::string& path);
std::optional<Config> try_load_default_config();
