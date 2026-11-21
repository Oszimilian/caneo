#include "config/AppConfig.hpp"
#include "compat/print.hpp"
#include "logger/LogReader.hpp"
#include "modes/config_picker.hpp"
#if CANEO_HAS_GUI
#include "modes/run_gui.hpp"
#endif
#include "modes/run_live.hpp"
#include "modes/run_playback.hpp"
#include "modes/run_tui.hpp"
#include "setup/InterfaceSetup.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <unistd.h>
#include <vector>

int main(int argc, char* argv[]) {
    AppConfig app;
    try {
        app = AppConfig::parse(argc, argv);
    } catch (const std::exception& e) {
        std::println(stderr, "Error loading config: {}", e.what());
        return 1;
    }

    // In playback mode: auto-detect interfaces from the log if none were specified.
    if (!app.playback_path.empty() && app.config.interfaces.empty()) {
        std::unique_ptr<LogReader> reader;
        try {
            reader = open_log_reader(app.playback_path);
        } catch (const std::exception& e) {
            std::println(stderr, "Error: {}", e.what());
            return 1;
        }
        for (const auto& iface : reader->scan_interfaces()) {
            InterfaceConfig cfg;
            cfg.name = iface;
            app.config.interfaces.push_back(std::move(cfg));
        }
    }

    // Terminal UI without a config: let the user search one (the GUI opens its own dialog).
    if (!app.config_found && app.playback_path.empty() && app.tui_mode) {
        const auto chosen = pick_config_tui();
        if (!chosen) return 0;
        try {
            app.config      = load_config(*chosen);
            app.config_path = *chosen;
        } catch (const std::exception& e) {
            std::println(stderr, "Error loading config: {}", e.what());
            return 1;
        }
    }

    // The GUI can start without interfaces: channels are set up in Settings.
    if (app.config.interfaces.empty() && app.playback_path.empty() && !app.gui_mode) {
        std::println(stderr, "Error: no interfaces specified and no caneo.yaml found.");
        return 1;
    }

    // The GUI sets the interfaces up when "Start" is pressed (no password prompt before that).
    if (!app.gui_mode && !app.config.interfaces.empty() && app.grpc_client.empty()) {
        try {
            setup_interfaces(app.config);
        } catch (const std::exception& e) {
            std::println(stderr, "Error setting up interfaces: {}", e.what());
            return 1;
        }
    }

    bool restart = false;
    try {
        if (!app.playback_path.empty()) run_playback(app);
        else if (app.tui_mode)          run_tui(app);
#if CANEO_HAS_GUI
        else if (app.gui_mode)          restart = run_gui(app);
#else
        else if (app.gui_mode)          throw std::runtime_error("built without GUI (CANEO_GUI=OFF), use --tui or --headless");
#endif
        else                            run_live(app);
    } catch (const std::exception& e) {
        std::println(stderr, "Error: {}", e.what());
        return 1;
    }

    if (restart) {
        // Everything is shut down at this point (sockets closed, log flushed):
        // replace the process with a fresh caneo using the saved config.
        const auto args = app.restart_args();
        std::vector<char*> argv_new;
        for (const auto& a : args)
            argv_new.push_back(const_cast<char*>(a.c_str()));
        argv_new.push_back(nullptr);
        std::println("Restarting with {}", app.config_path);
        execv("/proc/self/exe", argv_new.data());
        std::println(stderr, "Restart failed: {}", std::strerror(errno));
        return 1;
    }
}
