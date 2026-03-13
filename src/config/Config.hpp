#pragma once

#include <optional>
#include <string>
#include <vector>

struct InterfaceConfig {
    std::string name;
    std::string dbc; // empty if not set
};

std::vector<InterfaceConfig> load_config(const std::string& path);
std::optional<std::vector<InterfaceConfig>> try_load_default_config();
