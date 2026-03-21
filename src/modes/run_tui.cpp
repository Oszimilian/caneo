#include "run_tui.hpp"
#include "log_frame.hpp"

#include "action/ActionHandler.hpp"
#include "action/ActionPreset.hpp"
#include "compat/print.hpp"
#include "decoder/DecoderRegistry.hpp"
#include "frame/CanFrame.hpp"
#include "frame/DataFrame.hpp"
#include "gui/TuiDataFrameSet.hpp"
#include "logger/McapLogger.hpp"
#include "model/ModelEngine.hpp"
#include "model/SignalStore.hpp"
#include "proto/ProtoLogRegistry.hpp"
#include "grpc/CanStreamServer.hpp"
#include "socket/Socket.hpp"
#include "socket/SocketCAN.hpp"
#include "socket/SocketGrpc.hpp"
#include <algorithm>

#include <boost/asio.hpp>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

void run_tui(const AppConfig& app) {
    boost::asio::io_context io;
    DecoderRegistry decoders;
    std::vector<std::unique_ptr<Socket>> sockets;
    std::map<std::string, SocketCAN*> socket_map;  // only populated in SocketCAN mode

    SendFn send_fn = [&socket_map](const std::string& iface, uint64_t id,
                                   const std::vector<uint8_t>& data) {
        if (auto it = socket_map.find(iface); it != socket_map.end())
            it->second->send(id, data);
    };

    ActionHandler action_handler(io, send_fn);

    // Load per-interface action presets (all start paused)
    for (const auto& cfg : app.config.interfaces)
        load_action_presets(cfg, action_handler);

    auto tui = std::make_shared<TuiDataFrameSet>(app.config.interfaces, action_handler);

    ProtoLogRegistry proto_registry;
    std::unique_ptr<Logger> logger;
    if (app.log_mode) {
        for (const auto& cfg : app.config.interfaces)
            if (!cfg.dbc.empty())
                proto_registry.add_interface(cfg.name, cfg.dbc);
        logger = std::make_unique<McapLogger>(app.make_log_path());
    }

    std::unique_ptr<CanStreamServer> grpc_server;
    if (app.grpc_server)
        grpc_server = std::make_unique<CanStreamServer>(app.grpc_port);

    FrameTimestampMap last_frame_ts;
    SignalStore signal_store;
    std::unique_ptr<ModelEngine> model_engine;
    if (!app.model_path.empty())
        model_engine = std::make_unique<ModelEngine>(app.model_path, signal_store, logger.get());

    for (const auto& cfg : app.config.interfaces) {
        if (!cfg.dbc.empty())
            decoders.add_interface(cfg.name, cfg.dbc);
        std::unique_ptr<Socket> sock;
        if (app.grpc_client.empty()) {
            auto* can = new SocketCAN(io, cfg.name);
            socket_map[cfg.name] = can;
            sock.reset(can);
        } else {
            sock = std::make_unique<SocketGrpc>(io, cfg.name, app.grpc_client);
        }
        auto& socket = sockets.emplace_back(std::move(sock));
        socket->onFrame([tui, &decoders, &proto_registry, &logger,
                         &last_frame_ts, &model_engine, &grpc_server]
                        (std::unique_ptr<DataFrame> frame) {
            auto* f = dynamic_cast<CanFrame*>(frame.get());
            if (!f) return;
            if (grpc_server) grpc_server->broadcast(*f);
            try { decoders.decode(*f); } catch (const std::runtime_error&) {}
            tui->update(*f);
            if (model_engine) model_engine->on_frame(*f);
            log_frame(*f, logger.get(), proto_registry, last_frame_ts);
        });
        socket->start();
    }

    if (!app.grpc_client.empty()) {
        std::vector<SocketGrpc*> grpc_sockets;
        for (auto& s : sockets)
            if (auto* gs = dynamic_cast<SocketGrpc*>(s.get()))
                grpc_sockets.push_back(gs);
        if (!grpc_sockets.empty()) {
            tui->set_status_indicator(app.grpc_client, [grpc_sockets]() {
                return std::any_of(grpc_sockets.begin(), grpc_sockets.end(),
                                   [](SocketGrpc* s) { return s->connected(); });
            });
        }
    }

    // Keep io.run() alive even when no async CAN reads are pending (e.g. grpc_client mode).
    auto work_guard = boost::asio::make_work_guard(io);
    std::thread asio_thread([&io] { io.run(); });
    tui->run();
    work_guard.reset();
    io.stop();
    asio_thread.join();
    logger.reset();
}
