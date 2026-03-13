#include "Config.hpp"

#include <filesystem>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

static std::vector<InterfaceConfig> parse_config(const YAML::Node& root) {
    std::vector<InterfaceConfig> result;
    const auto& interfaces = root["interfaces"];
    if (!interfaces)
        throw std::runtime_error("Config: missing 'interfaces' key");

    for (const auto& entry : interfaces) {
        InterfaceConfig cfg;
        cfg.name = entry.first.as<std::string>();
        if (entry.second && entry.second.IsScalar())
            cfg.dbc = entry.second.as<std::string>();
        result.push_back(std::move(cfg));
    }
    return result;
}

std::vector<InterfaceConfig> load_config(const std::string& path) {
    return parse_config(YAML::LoadFile(path));
}

std::optional<std::vector<InterfaceConfig>> try_load_default_config() {
    const std::filesystem::path p = "caneo.yaml";
    if (!std::filesystem::exists(p))
        return std::nullopt;
    return load_config(p.string());
}
