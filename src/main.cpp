#include "config/AppConfig.hpp"
#include "compat/print.hpp"
#include "logger/McapReader.hpp"
#include "modes/run_live.hpp"
#include "modes/run_playback.hpp"
#include "modes/run_tui.hpp"
#include "setup/InterfaceSetup.hpp"

int main(int argc, char* argv[]) {
    AppConfig app;
    try {
        app = AppConfig::parse(argc, argv);
    } catch (const std::exception& e) {
        std::println(stderr, "Error loading config: {}", e.what());
        return 1;
    }

    // In playback mode: auto-detect interfaces from MCAP if none were specified.
    if (!app.playback_path.empty() && app.config.interfaces.empty()) {
        for (const auto& iface : McapReader(app.playback_path).scan_interfaces()) {
            InterfaceConfig cfg;
            cfg.name = iface;
            app.config.interfaces.push_back(std::move(cfg));
        }
    }

    if (app.config.interfaces.empty() && app.playback_path.empty()) {
        std::println(stderr, "Error: no interfaces specified and no caneo.yaml found.");
        return 1;
    }

    if (!app.config.interfaces.empty()) {
        try {
            setup_interfaces(app.config);
        } catch (const std::exception& e) {
            std::println(stderr, "Error setting up interfaces: {}", e.what());
            return 1;
        }
    }

    try {
        if (!app.playback_path.empty()) run_playback(app);
        else if (app.tui_mode)          run_tui(app);
        else                            run_live(app);
    } catch (const std::exception& e) {
        std::println(stderr, "Error: {}", e.what());
        return 1;
    }
}
