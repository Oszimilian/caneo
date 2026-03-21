#include "AppConfig.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <gflags/gflags.h>
#include <vector>

DEFINE_bool(tui,      false, "Enable TUI mode");
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
        "caneo [--tui] [--log] [--config FILE] [--model FILE] [--playback FILE] [iface[:dbc] ...]");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    AppConfig app;
    app.tui_mode      = FLAGS_tui;
    app.gui_mode      = FLAGS_gui;
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
        app.config = load_config(FLAGS_config);
    } else if (iface_args.empty()) {
        if (auto cfg = try_load_default_config())
            app.config = std::move(*cfg);
    } else {
        for (const auto& arg : iface_args) {
            const auto sep = arg.find(':');
            InterfaceConfig iface;
            iface.name = arg.substr(0, sep);
            if (sep != std::string::npos)
                iface.dbc = arg.substr(sep + 1);
            app.config.interfaces.push_back(std::move(iface));
        }
    }

    return app;
}

std::string AppConfig::make_log_path() const {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    char name[32];
    std::strftime(name, sizeof(name), "caneo_%Y%m%d_%H%M%S.mcap", std::localtime(&t));

    std::filesystem::path dir = config.log_file_path.value_or(std::filesystem::current_path());
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}
