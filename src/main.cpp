#include "action/ActionHandler.hpp"
#include "compat/print.hpp"
#include "config/Config.hpp"
#include "decoder/DecoderRegistry.hpp"
#include "frame/CanFrame.hpp"
#include "frame/DataFrameSet.hpp"
#include "gui/TuiDataFrameSet.hpp"
#include "logger/McapLogger.hpp"
#include "logger/McapReader.hpp"
#include "logger/Logger.hpp"
#include "model/ModelEngine.hpp"
#include "model/SignalStore.hpp"
#include "proto/ProtoLogRegistry.hpp"
#include "setup/InterfaceSetup.hpp"
#include "socket/SocketCAN.hpp"

#include <map>

#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <chrono>
#include <ctime>
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
    bool debug_mode = false;
    std::string config_path;
    std::string model_path;
    std::string playback_path;
    std::vector<std::string_view> iface_args;

    const auto args = std::span(argv + 1, argc - 1);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--tui") {
            tui_mode = true;
        } else if (arg == "--log") {
            log_mode = true;
        } else if (arg == "--debug") {
            debug_mode = true;
        } else if (arg == "--config") {
            if (i + 1 >= args.size()) {
                std::println(stderr, "Error: --config requires a file path.");
                return 1;
            }
            config_path = args[++i];
        } else if (arg == "--model") {
            if (i + 1 >= args.size()) {
                std::println(stderr, "Error: --model requires a file path.");
                return 1;
            }
            model_path = args[++i];
        } else if (arg == "--playback") {
            if (i + 1 >= args.size()) {
                std::println(stderr, "Error: --playback requires a file path.");
                return 1;
            }
            playback_path = args[++i];
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

    // In playback mode: auto-detect interfaces from MCAP if none were specified.
    if (!playback_path.empty() && config.interfaces.empty()) {
        for (const auto& iface : McapReader(playback_path).scan_interfaces()) {
            InterfaceConfig cfg;
            cfg.name = iface;
            config.interfaces.push_back(std::move(cfg));
        }
    }

    if (config.interfaces.empty() && playback_path.empty()) {
        std::println(stderr, "Error: no interfaces specified and no caneo.yaml found.");
        return 1;
    }

    if (playback_path.empty()) {
        try {
            setup_interfaces(config);
        } catch (const std::exception& e) {
            std::println(stderr, "Error setting up interfaces: {}", e.what());
            return 1;
        }
    }

    const auto& iface_configs = config.interfaces;

    boost::asio::io_context io;
    DecoderRegistry decoders;
    std::vector<std::unique_ptr<SocketCAN>> sockets;

    // ── Shared helper: build socket_map for sending ──────────────────────────
    std::map<std::string, SocketCAN*> socket_map;

    if (!playback_path.empty()) {
        // ════════════════════════════════════════════════════════════════════
        // PLAYBACK MODE  (--playback file.mcap)
        // Opens sockets for each interface (to send), then replays the MCAP.
        // Optional: --tui shows a live trace of what is being replayed.
        // ════════════════════════════════════════════════════════════════════
        for (const auto& cfg : iface_configs) {
            try {
                auto& socket = sockets.emplace_back(
                    std::make_unique<SocketCAN>(io, cfg.name));
                socket_map[cfg.name] = socket.get();
            } catch (const std::exception& e) {
                std::println(stderr, "Warning: cannot open {}: {}", cfg.name, e.what());
            }
        }

        McapReader::OnSend on_send = [&socket_map](const std::string& iface,
                                                    uint32_t can_id,
                                                    const std::vector<uint8_t>& payload) {
            if (auto it = socket_map.find(iface); it != socket_map.end())
                it->second->send(can_id, payload);
        };

        if (tui_mode) {
            SendFn send_fn = [&socket_map](const std::string& iface, uint64_t id,
                                           const std::vector<uint8_t>& data) {
                if (auto it = socket_map.find(iface); it != socket_map.end())
                    it->second->send(id, data);
            };
            ActionHandler action_handler(io, send_fn);
            auto tui = std::make_shared<TuiDataFrameSet>(iface_configs, action_handler);

            std::thread playback_thread([&playback_path, tui, &on_send] {
                try {
                    McapReader reader(playback_path);
                    reader.play(
                        [&tui](std::unique_ptr<CanFrame> frame) { tui->update(*frame); },
                        on_send);
                } catch (const std::exception& e) {
                    std::println(stderr, "Playback error: {}", e.what());
                }
            });

            std::thread asio_thread([&io] { io.run(); });
            tui->run();
            io.stop();
            asio_thread.join();
            playback_thread.join();
        } else {
            // No TUI — just replay on the bus and block until done.
            std::thread asio_thread([&io] { io.run(); });
            try {
                McapReader reader(playback_path);
                reader.play({}, on_send);  // no on_frame needed without TUI
            } catch (const std::exception& e) {
                std::println(stderr, "Playback error: {}", e.what());
            }
            io.stop();
            asio_thread.join();
        }

    } else if (tui_mode) {
        // ════════════════════════════════════════════════════════════════════
        // LIVE TUI MODE  (--tui)
        // ════════════════════════════════════════════════════════════════════
        SendFn send_fn = [&socket_map](const std::string& iface, uint64_t id,
                                       const std::vector<uint8_t>& data) {
            if (auto it = socket_map.find(iface); it != socket_map.end())
                it->second->send(id, data);
        };

        ActionHandler action_handler(io, send_fn);
        auto tui = std::make_shared<TuiDataFrameSet>(iface_configs, action_handler);

        ProtoLogRegistry proto_registry;
        std::unique_ptr<Logger> logger;
        if (log_mode) {
            for (const auto& cfg : iface_configs)
                if (!cfg.dbc.empty())
                    proto_registry.add_interface(cfg.name, cfg.dbc);

            const auto now = std::chrono::system_clock::now();
            const std::time_t t = std::chrono::system_clock::to_time_t(now);
            char buf[32];
            std::strftime(buf, sizeof(buf), "caneo_%Y%m%d_%H%M%S.mcap", std::localtime(&t));
            logger = std::make_unique<McapLogger>(buf);
        }

        std::map<std::pair<std::string, uint32_t>,
                 std::chrono::steady_clock::time_point> last_frame_ts;

        SignalStore signal_store;
        std::unique_ptr<ModelEngine> model_engine;
        if (!model_path.empty()) {
            try {
                model_engine = std::make_unique<ModelEngine>(
                    model_path, signal_store, logger.get());
            } catch (const std::exception& e) {
                std::println(stderr, "Error loading model: {}", e.what());
                return 1;
            }
        }

        for (const auto& cfg : iface_configs) {
            if (!cfg.dbc.empty())
                decoders.add_interface(cfg.name, cfg.dbc);
            auto& socket = sockets.emplace_back(std::make_unique<SocketCAN>(io, cfg.name));
            socket_map[cfg.name] = socket.get();
            socket->onFrame([tui, &decoders, &proto_registry, &logger, &last_frame_ts, &model_engine](std::unique_ptr<DataFrame> frame) {
                if (auto* canFrame = dynamic_cast<CanFrame*>(frame.get())) {
                    try { decoders.decode(*canFrame); } catch (const std::runtime_error&) {}
                    tui->update(*canFrame);
                    if (model_engine)
                        model_engine->on_frame(*canFrame);
                    if (logger) {
                        const std::string serialized = proto_registry.serialize(*canFrame);
                        const auto* proto_log = proto_registry.get(canFrame->header().interface);
                        const auto* desc = proto_log
                            ? proto_log->descriptor(canFrame->header().id)
                            : nullptr;

                        const auto key = std::make_pair(
                            canFrame->header().interface, canFrame->header().id);
                        std::optional<double> ratio_ms;
                        if (const auto it = last_frame_ts.find(key);
                            it != last_frame_ts.end())
                            ratio_ms = std::chrono::duration<double, std::milli>(
                                canFrame->timestamp() - it->second).count();
                        last_frame_ts[key] = canFrame->timestamp();

                        logger->log(*canFrame, desc, serialized, ratio_ms);
                    }
                }
            });
            socket->start();
        }

        std::thread asio_thread([&io] { io.run(); });
        tui->run();
        io.stop();
        asio_thread.join();
        logger.reset();

    } else {
        // ════════════════════════════════════════════════════════════════════
        // LIVE NON-TUI MODE  (default)
        // ════════════════════════════════════════════════════════════════════
        std::vector<DataFrameSet> sets;
        ProtoLogRegistry proto_registry;

        for (const auto& cfg : iface_configs) {
            sets.emplace_back(cfg.name);
            if (!cfg.dbc.empty()) {
                decoders.add_interface(cfg.name, cfg.dbc);
                proto_registry.add_interface(cfg.name, cfg.dbc);
            }
        }

        std::unique_ptr<Logger> logger;
        if (log_mode) {
            const auto now = std::chrono::system_clock::now();
            const std::time_t t = std::chrono::system_clock::to_time_t(now);
            char buf[32];
            std::strftime(buf, sizeof(buf), "caneo_%Y%m%d_%H%M%S.mcap", std::localtime(&t));
            logger = std::make_unique<McapLogger>(buf);
        }

        std::map<std::pair<std::string, uint32_t>,
                 std::chrono::steady_clock::time_point> last_frame_ts;

        SignalStore signal_store;
        std::unique_ptr<ModelEngine> model_engine;
        if (!model_path.empty()) {
            try {
                model_engine = std::make_unique<ModelEngine>(
                    model_path, signal_store, logger.get());
            } catch (const std::exception& e) {
                std::println(stderr, "Error loading model: {}", e.what());
                return 1;
            }
        }

        for (std::size_t i = 0; i < sets.size(); ++i) {
            auto& socket = sockets.emplace_back(
                std::make_unique<SocketCAN>(io, sets[i].interface()));
            socket->onFrame([&sets, &decoders, &proto_registry, &logger, &debug_mode, &last_frame_ts, &model_engine, i](std::unique_ptr<DataFrame> frame) {
                auto* canFrame = dynamic_cast<CanFrame*>(frame.get());
                if (canFrame) {
                    try { decoders.decode(*canFrame); } catch (const std::runtime_error&) {}
                    sets[i].update(*canFrame);
                    if (model_engine)
                        model_engine->on_frame(*canFrame);
                }
                if (debug_mode) {
                    for (const auto& set : sets)
                        std::println("{}", set);
                    std::println("--------------------------------------------------");
                }
                if (canFrame) {
                    if (logger) {
                        const std::string serialized = proto_registry.serialize(*canFrame);
                        const auto* proto_log = proto_registry.get(canFrame->header().interface);
                        const auto* desc = proto_log
                            ? proto_log->descriptor(canFrame->header().id)
                            : nullptr;

                        const auto key = std::make_pair(
                            canFrame->header().interface, canFrame->header().id);
                        std::optional<double> ratio_ms;
                        if (const auto it = last_frame_ts.find(key);
                            it != last_frame_ts.end())
                            ratio_ms = std::chrono::duration<double, std::milli>(
                                canFrame->timestamp() - it->second).count();
                        last_frame_ts[key] = canFrame->timestamp();

                        logger->log(*canFrame, desc, serialized, ratio_ms);
                    } else if (debug_mode) {
                        const std::string description = proto_registry.describe(*canFrame);
                        if (!description.empty())
                            std::println("proto:\n{}", description);
                    }
                }
            });
            socket->start();
            std::println("Listening on {}...", *socket);
        }

        boost::asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&io, &logger](const boost::system::error_code&, int) {
            logger.reset();
            io.stop();
        });

        io.run();
    }
}
