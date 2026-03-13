#include "compat/print.hpp"
#include "config/Config.hpp"
#include "decoder/DecoderRegistry.hpp"
#include "frame/CanFrame.hpp"
#include "frame/DataFrameSet.hpp"
#include "gui/TuiDataFrameSet.hpp"
#include "socket/SocketCAN.hpp"

#include <boost/asio.hpp>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::println(stderr, "Usage: caneo [--tui] [--config <file>] [<interface>[:<dbc>] ...]");
        std::println(stderr, "Example: caneo --tui vcan0:vcan0.dbc vcan1");
        std::println(stderr, "         caneo --tui --config caneo.yaml");
        return 1;
    }

    bool tui_mode = false;
    std::string config_path;
    std::vector<std::string_view> iface_args;

    const auto args = std::span(argv + 1, argc - 1);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--tui") {
            tui_mode = true;
        } else if (arg == "--config") {
            if (i + 1 >= args.size()) {
                std::println(stderr, "Error: --config requires a file path.");
                return 1;
            }
            config_path = args[++i];
        } else {
            iface_args.push_back(arg);
        }
    }

    // Resolve interface configs
    std::vector<InterfaceConfig> iface_configs;
    try {
        if (!config_path.empty()) {
            iface_configs = load_config(config_path);
        } else if (iface_args.empty()) {
            // No interfaces given — try caneo.yaml in cwd
            if (auto cfg = try_load_default_config()) {
                iface_configs = std::move(*cfg);
            }
        } else {
            // Explicit interface args — parse them directly, skip caneo.yaml
            for (const std::string_view arg : iface_args) {
                const auto sep = arg.find(':');
                InterfaceConfig cfg;
                cfg.name = std::string(arg.substr(0, sep));
                if (sep != std::string_view::npos)
                    cfg.dbc = std::string(arg.substr(sep + 1));
                iface_configs.push_back(std::move(cfg));
            }
        }
    } catch (const std::exception& e) {
        std::println(stderr, "Error loading config: {}", e.what());
        return 1;
    }

    if (iface_configs.empty()) {
        std::println(stderr, "Error: no interfaces specified and no caneo.yaml found.");
        return 1;
    }

    boost::asio::io_context io;
    DecoderRegistry decoders;
    std::vector<std::unique_ptr<SocketCAN>> sockets;

    if (tui_mode) {
        auto tui = std::make_shared<TuiDataFrameSet>();

        for (const auto& cfg : iface_configs) {
            if (!cfg.dbc.empty())
                decoders.add_interface(cfg.name, cfg.dbc);
            auto& socket = sockets.emplace_back(std::make_unique<SocketCAN>(io, cfg.name));
            socket->onFrame([tui, &decoders](std::unique_ptr<DataFrame> frame) {
                if (auto* canFrame = dynamic_cast<CanFrame*>(frame.get())) {
                    try { decoders.decode(*canFrame); } catch (const std::runtime_error&) {}
                    tui->update(*canFrame);
                }
            });
            socket->start();
        }

        std::thread asio_thread([&io] { io.run(); });
        tui->run();
        io.stop();
        asio_thread.join();

    } else {
        std::vector<DataFrameSet> sets;

        for (const auto& cfg : iface_configs) {
            sets.emplace_back(cfg.name);
            if (!cfg.dbc.empty())
                decoders.add_interface(cfg.name, cfg.dbc);
        }

        for (std::size_t i = 0; i < sets.size(); ++i) {
            auto& socket = sockets.emplace_back(std::make_unique<SocketCAN>(io, sets[i].interface()));
            socket->onFrame([&sets, &decoders, i](std::unique_ptr<DataFrame> frame) {
                if (auto* canFrame = dynamic_cast<CanFrame*>(frame.get())) {
                    try { decoders.decode(*canFrame); } catch (const std::runtime_error&) {}
                    sets[i].update(*canFrame);
                }
                for (const auto& set : sets) {
                    std::println("{}", set);
                }
                std::println("--------------------------------------------------");
            });
            socket->start();
            std::println("Listening on {}...", *socket);
        }

        io.run();
    }
}
