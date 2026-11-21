#include "AppConfig.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <gflags/gflags.h>
#include <vector>

DEFINE_bool(tui,      false, "Enable TUI mode");
DEFINE_bool(headless, false, "No UI: print frames / log to the terminal");
DEFINE_bool(gui,      false, "Enable GUI mode (Dear ImGui)");
DEFINE_bool(log,      false, "Enable logging to MCAP file");
DEFINE_bool(debug,    false, "Enable debug output");
DEFINE_bool(grpc_server, false, "Stream CAN frames to gRPC clients");
DEFINE_string(grpc_client, "", "Receive CAN frames from gRPC server (host:port)");
DEFINE_uint32(grpc_port, 50051, "gRPC server port");
DEFINE_string(config,   "", "Path to caneo.yaml config file");
DEFINE_string(model,    "", "Path to Lua model script");
DEFINE_string(playback, "", "Path to MCAP file for playback");

AppConfig AppConfig::parse(int& argc, char* argv[]) {
    gflags::SetUsageMessage(
        "caneo [--tui | --gui | --headless] [--log] [--config FILE] [--model FILE] [--playback FILE] [iface[:dbc] ...]");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    AppConfig app;
    app.tui_mode      = FLAGS_tui;
    app.gui_mode      = FLAGS_gui;
    app.headless_mode = FLAGS_headless;
    app.log_mode      = FLAGS_log;
    app.debug_mode    = FLAGS_debug;
    app.grpc_server   = FLAGS_grpc_server;
    app.grpc_client   = FLAGS_grpc_client;
    app.grpc_port     = static_cast<uint16_t>(FLAGS_grpc_port);
    app.model_path    = FLAGS_model;
    app.playback_path = FLAGS_playback;

    // Remaining positional args are interface specs (e.g. vcan0:dbc/file.dbc)
    const std::vector<std::string> iface_args(argv + 1, argv + argc);

    if (!FLAGS_config.empty()) {
        app.config_path = FLAGS_config;
        app.config      = load_config(FLAGS_config);
        app.config_found = true;
    } else if (iface_args.empty()) {
        if (auto cfg = try_load_default_config()) {
            app.config = std::move(*cfg);
            app.config_found = true;
        }
    } else {
        for (const auto& arg : iface_args) {
            const auto sep = arg.find(':');
            InterfaceConfig iface;
            iface.name = arg.substr(0, sep);
            if (sep != std::string::npos)
                iface.dbc = arg.substr(sep + 1);
            app.config.interfaces.push_back(std::move(iface));
        }
        app.config_found = true;
    }

    // Without a mode flag the GUI starts (--tui for the terminal UI);
    // a build without GUI (CANEO_GUI=OFF) runs headless instead.
    if (!app.gui_mode && !app.tui_mode && !app.headless_mode && app.playback_path.empty()) {
#if !defined(CANEO_HAS_GUI) || CANEO_HAS_GUI
        app.gui_mode = true;
#else
        app.headless_mode = true;
#endif
    }

    return app;
}

std::string AppConfig::log_file_name() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    char name[32];
    std::strftime(name, sizeof(name), "caneo_%Y%m%d_%H%M%S.mcap", std::localtime(&t));
    return name;
}

std::string AppConfig::make_log_path() const {
    const std::string name = log_file_name();

    std::filesystem::path dir = config.log_dir().value_or(std::filesystem::current_path());
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

std::vector<std::string> AppConfig::restart_args() const {
    std::vector<std::string> args{"caneo"};
    if (tui_mode)            args.emplace_back("--tui");
    if (gui_mode)            args.emplace_back("--gui");
    if (headless_mode)       args.emplace_back("--headless");
    if (log_mode)            args.emplace_back("--log");
    if (debug_mode)          args.emplace_back("--debug");
    if (grpc_server)         args.emplace_back("--grpc_server");
    if (!grpc_client.empty()) args.push_back("--grpc_client=" + grpc_client);
    args.push_back("--grpc_port=" + std::to_string(grpc_port));
    if (!model_path.empty()) args.push_back("--model=" + model_path);
    args.push_back("--config=" + config_path);
    return args;
}
