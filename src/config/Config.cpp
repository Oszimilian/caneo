#include "Config.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

static std::filesystem::path expand_path(const std::string& raw) {
    if (raw == ".")
        return std::filesystem::current_path();
    if (raw.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        if (home)
            return std::filesystem::path(home) / raw.substr(2);
    }
    return raw;
}

static Config parse_config(const YAML::Node& root) {
    Config result;

    if (root["virtual"])
        result.virtual_can = root["virtual"].as<bool>();

    if (root["log_file_path"])
        result.log_file_path = expand_path(root["log_file_path"].as<std::string>());

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
                if (entry.second["actions"])
                    cfg.actions_file = entry.second["actions"].as<std::string>();
                if (entry.second["graphs_preset"])
                    cfg.graphs_file = entry.second["graphs_preset"].as<std::string>();
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
