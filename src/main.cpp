#include "action/ActionHandler.hpp"
#include "compat/print.hpp"
#include "config/Config.hpp"
#include "decoder/DecoderRegistry.hpp"
#include "frame/CanFrame.hpp"
#include "frame/DataFrameSet.hpp"
#include "gui/TuiDataFrameSet.hpp"
#include "proto/ProtoLogRegistry.hpp"
#include "setup/InterfaceSetup.hpp"
#include "socket/SocketCAN.hpp"

#include <map>

#include <boost/asio.hpp>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

int main(int argc, char* argv[]) {

    bool tui_mode = false;
    bool log_mode = false;
    std::string config_path;
    std::vector<std::string_view> iface_args;

    const auto args = std::span(argv + 1, argc - 1);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--tui") {
            tui_mode = true;
        } else if (arg == "--log") {
            log_mode = true;
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
    Config config;
    try {
        if (!config_path.empty()) {
            config = load_config(config_path);
        } else if (iface_args.empty()) {
            // No interfaces given — try caneo.yaml in cwd
            if (auto cfg = try_load_default_config()) {
                config = std::move(*cfg);
            }
        } else {
            // Explicit interface args — parse them directly, skip caneo.yaml
            for (const std::string_view arg : iface_args) {
                const auto sep = arg.find(':');
                InterfaceConfig iface;
                iface.name = std::string(arg.substr(0, sep));
                if (sep != std::string_view::npos)
                    iface.dbc = std::string(arg.substr(sep + 1));
                config.interfaces.push_back(std::move(iface));
            }
        }
    } catch (const std::exception& e) {
        std::println(stderr, "Error loading config: {}", e.what());
        return 1;
    }

    if (config.interfaces.empty()) {
        std::println(stderr, "Error: no interfaces specified and no caneo.yaml found.");
        return 1;
    }

    try {
        setup_interfaces(config);
    } catch (const std::exception& e) {
        std::println(stderr, "Error setting up interfaces: {}", e.what());
        return 1;
    }

    const auto& iface_configs = config.interfaces;

    boost::asio::io_context io;
    DecoderRegistry decoders;
    std::vector<std::unique_ptr<SocketCAN>> sockets;

    if (tui_mode) {
        // Build a socket map so the send function can look up sockets by interface name.
        // The map is populated before io.run() starts, so it's safe to read from the asio thread.
        std::map<std::string, SocketCAN*> socket_map;

        SendFn send_fn = [&socket_map](const std::string& iface, uint64_t id,
                                       const std::vector<uint8_t>& data) {
            auto it = socket_map.find(iface);
            if (it != socket_map.end())
                it->second->send(id, data);
        };

        ActionHandler action_handler(io, send_fn);
        auto tui = std::make_shared<TuiDataFrameSet>(iface_configs, action_handler);

        for (const auto& cfg : iface_configs) {
            if (!cfg.dbc.empty())
                decoders.add_interface(cfg.name, cfg.dbc);
            auto& socket = sockets.emplace_back(std::make_unique<SocketCAN>(io, cfg.name));
            socket_map[cfg.name] = socket.get();
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
        ProtoLogRegistry proto_registry;

        for (const auto& cfg : iface_configs) {
            sets.emplace_back(cfg.name);
            if (!cfg.dbc.empty()) {
                decoders.add_interface(cfg.name, cfg.dbc);
                proto_registry.add_interface(cfg.name, cfg.dbc);
            }
        }

        for (std::size_t i = 0; i < sets.size(); ++i) {
            auto& socket = sockets.emplace_back(std::make_unique<SocketCAN>(io, sets[i].interface()));
            socket->onFrame([&sets, &decoders, &proto_registry, &log_mode, i](std::unique_ptr<DataFrame> frame) {
                auto* canFrame = dynamic_cast<CanFrame*>(frame.get());
                if (canFrame) {
                    try { decoders.decode(*canFrame); } catch (const std::runtime_error&) {}
                    sets[i].update(*canFrame);
                }
                for (const auto& set : sets) {
                    std::println("{}", set);
                }
                std::println("--------------------------------------------------");
                if (!log_mode && canFrame) {
                    const std::string description = proto_registry.describe(*canFrame);
                    if (!description.empty())
                        std::println("proto:\n{}", description);
                }
            });
            socket->start();
            std::println("Listening on {}...", *socket);
        }

        io.run();
    }
}
