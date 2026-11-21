#include "run_tui.hpp"

#include "action/ActionHandler.hpp"
#include "action/ActionFactory.hpp"
#include "compat/print.hpp"
#include "decoder/DecoderRegistry.hpp"
#include "frame/CanFrame.hpp"
#include "frame/DataFrame.hpp"
#include "gui/TuiDataFrameSet.hpp"
#include "logger/LogController.hpp"
#include "model/ModelEngine.hpp"
#include "model/SignalStore.hpp"
#include "proto/ProtoLogRegistry.hpp"
#include "grpc/CanStreamServer.hpp"
#include "guard/GuardEngine.hpp"
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
    std::map<std::string, SocketCAN*>  socket_map;       // populated in SocketCAN mode
    std::map<std::string, SocketGrpc*> grpc_socket_map;  // populated in grpc_client mode

    SendFn send_fn = [&socket_map, &grpc_socket_map](const std::string& iface, uint64_t id,
                                                      const std::vector<uint8_t>& data) {
        if (auto it = grpc_socket_map.find(iface); it != grpc_socket_map.end())
            it->second->send(id, data);
        else if (auto it = socket_map.find(iface); it != socket_map.end())
            it->second->send(id, data);
    };

    ActionHandler action_handler(io, send_fn);

    // Actions from the config (paused unless saved as running)
    ActionFactory action_factory(action_handler);
    action_factory.set_interfaces(app.config.interfaces);
    for (const auto& err : action_factory.add_all(app.config.interfaces).errors)
        std::println(stderr, "Action skipped: {}", err);

    auto tui = std::make_shared<TuiDataFrameSet>(app.config.interfaces, action_handler);

    ProtoLogRegistry proto_registry;
    for (const auto& cfg : app.config.interfaces)
        if (!cfg.dbc.empty())
            proto_registry.add_interface(cfg.name, cfg.dbc);
    // MCAP and/or BLF depending on logging.format
    LogController log_controller(app, proto_registry);
    log_controller.set_verbose(false);   // stdout belongs to ftxui
    if (app.log_mode)
        log_controller.start();
    tui->set_log_control(&log_controller, &app.config);
    tui->set_graphs(app.config.graphs);
    LogController* logger = &log_controller;

    std::unique_ptr<CanStreamServer> grpc_server;
    if (app.grpc_server) {
        grpc_server = std::make_unique<CanStreamServer>(app.grpc_port);
        grpc_server->set_send_fn(send_fn);
        grpc_server->set_interfaces(app.config.interfaces);
    }

    SignalStore signal_store;
    std::unique_ptr<ModelEngine> model_engine;
    if (!app.model_path.empty()) {
        model_engine = std::make_unique<ModelEngine>(app.model_path, signal_store, logger);
        tui->enable_model_tab();
        model_engine->set_output_callback([tui](const std::string& name, double value) {
            tui->push_model_output(name, value);
        });
    }

    // Guards: Guards tab ('g') + outputs logged as guard/<name>/<output>
    GuardEngine guard_engine(io, &signal_store);
    tui->set_guard_engine(&guard_engine);
    guard_engine.set_output_fn([logger, tui](const std::string& guard, const std::string& output, double v) {
        logger->log_scalar("guard/" + guard + "/" + output, v);
        tui->push_guard_output(guard, output, v);
    });
    guard_engine.add_all(app.config.guards);

    for (const auto& cfg : app.config.interfaces) {
        if (!cfg.dbc.empty())
            decoders.add_interface(cfg.name, cfg.dbc);
        std::unique_ptr<Socket> sock;
        if (app.grpc_client.empty() && !cfg.is_remote()) {
            auto* can = new SocketCAN(io, cfg.name);
            socket_map[cfg.name] = can;
            sock.reset(can);
        } else {
            auto* gs = cfg.is_remote() ? new SocketGrpc(io, cfg.name, cfg.grpc, cfg.remote_name())
                                          : new SocketGrpc(io, cfg.name, app.grpc_client);
            grpc_socket_map[cfg.name] = gs;
            sock.reset(gs);
        }
        auto& socket = sockets.emplace_back(std::move(sock));
        socket->onFrame([tui, &decoders, logger, &model_engine, &grpc_server, &signal_store, &guard_engine]
                        (std::unique_ptr<DataFrame> frame) {
            auto* f = dynamic_cast<CanFrame*>(frame.get());
            if (!f) return;
            if (grpc_server) grpc_server->broadcast(*f);
            try { decoders.decode(*f); } catch (const std::runtime_error&) {}
            tui->update(*f);
            if (model_engine) model_engine->on_frame(*f);   // updates signal_store too
            else              signal_store.update(*f);
            guard_engine.on_frame(*f);
            logger->log_can_frame(*f);
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
    log_controller.stop();
}
