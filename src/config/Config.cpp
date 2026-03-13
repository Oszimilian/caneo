#include "Config.hpp"

#include <filesystem>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

static Config parse_config(const YAML::Node& root) {
    Config result;

    if (root["virtual"])
        result.virtual_can = root["virtual"].as<bool>();

    const auto& interfaces = root["interfaces"];
    if (!interfaces)
        throw std::runtime_error("Config: missing 'interfaces' key");

    for (const auto& entry : interfaces) {
        InterfaceConfig cfg;
        cfg.name = entry.first.as<std::string>();
        if (entry.second) {
            if (entry.second.IsScalar()) {
                // Old format: vcan0: dbc/vcan0.dbc
                cfg.dbc = entry.second.as<std::string>();
            } else if (entry.second.IsMap()) {
                // New format: vcan0: { dbc: ..., baudrate: ... }
                if (entry.second["dbc"])
                    cfg.dbc = entry.second["dbc"].as<std::string>();
                if (entry.second["baudrate"])
                    cfg.baudrate = entry.second["baudrate"].as<uint32_t>();
            }
        }
        result.interfaces.push_back(std::move(cfg));
    }
    return result;
}

Config load_config(const std::string& path) {
    return parse_config(YAML::LoadFile(path));
}

std::optional<Config> try_load_default_config() {
    const std::filesystem::path p = "caneo.yaml";
    if (!std::filesystem::exists(p))
        return std::nullopt;
    return load_config(p.string());
}
