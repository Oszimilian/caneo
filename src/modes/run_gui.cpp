#include "run_gui.hpp"

#include "action/ActionHandler.hpp"
#include "action/ActionPreset.hpp"
#include "gui/GraphPreset.hpp"
#include "gui/ModelWindow.hpp"
#include "decoder/DecoderRegistry.hpp"
#include "frame/CanFrame.hpp"
#include "gui/Gui.hpp"
#include "logger/ILogControl.hpp"
#include "logger/LogController.hpp"
#include "grpc/RemoteLogControl.hpp"
#include "model/ModelEngine.hpp"
#include "model/SignalStore.hpp"
#include "proto/ProtoLogRegistry.hpp"
#include "grpc/CanStreamServer.hpp"
#include "socket/Socket.hpp"
#include "socket/SocketCAN.hpp"
#include "socket/SocketGrpc.hpp"
#include <algorithm>

#include <boost/asio.hpp>
#include <csignal>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

void run_gui(const AppConfig& app)
{
    boost::asio::io_context io;
    DecoderRegistry         decoders;
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

    // Load per-interface action presets (all start paused)
    for (const auto& cfg : app.config.interfaces)
        load_action_presets(cfg, action_handler);

    auto gui = std::make_shared<Gui>(app.config.interfaces, action_handler);

    // Load per-interface graph presets
    for (const auto& cfg : app.config.interfaces)
        load_graph_preset(cfg, *gui->graph_window());

    ProtoLogRegistry proto_registry;
    for (const auto& cfg : app.config.interfaces)
        if (!cfg.dbc.empty())
            proto_registry.add_interface(cfg.name, cfg.dbc);

    // In gRPC client mode the server owns the log; we control it remotely.
    // In local mode we log here directly.
    std::unique_ptr<ILogControl>  log_control;
    LogController*                local_lc = nullptr;  // only set in local mode

    if (app.grpc_client.empty()) {
        auto lc = std::make_unique<LogController>(app, proto_registry);
        if (app.log_mode) lc->start();
        local_lc    = lc.get();
        log_control = std::move(lc);
    } else {
        auto rc = std::make_unique<RemoteLogControl>(app.grpc_client);
        if (app.log_mode) rc->start();
        log_control = std::move(rc);
    }
    gui->set_log_controller(log_control.get());

    std::unique_ptr<CanStreamServer> grpc_server;
    if (app.grpc_server) {
        grpc_server = std::make_unique<CanStreamServer>(app.grpc_port);
        grpc_server->set_send_fn(send_fn);
    }

    SignalStore       signal_store;
    std::unique_ptr<ModelEngine> model_engine;
    if (!app.model_path.empty()) {
        model_engine = std::make_unique<ModelEngine>(app.model_path, signal_store, local_lc);
        auto* mw = gui->add_model_window();
        model_engine->set_output_callback([mw](const std::string& name, double value) {
            mw->push(name, value);
        });
    }

    for (const auto& cfg : app.config.interfaces) {
        if (!cfg.dbc.empty())
            decoders.add_interface(cfg.name, cfg.dbc);
        std::unique_ptr<Socket> sock;
        if (app.grpc_client.empty()) {
            auto* can = new SocketCAN(io, cfg.name);
            socket_map[cfg.name] = can;
            sock.reset(can);
        } else {
            auto* gs = new SocketGrpc(io, cfg.name, app.grpc_client);
            grpc_socket_map[cfg.name] = gs;
            sock.reset(gs);
        }
        auto& socket = sockets.emplace_back(std::move(sock));
        socket->onFrame([gui, &decoders, local_lc, &model_engine, &grpc_server]
                        (std::unique_ptr<DataFrame> frame) {
            auto* f = dynamic_cast<CanFrame*>(frame.get());
            if (!f) return;
            if (grpc_server) grpc_server->broadcast(*f);
            try { decoders.decode(*f); } catch (const std::runtime_error&) {}
            gui->update(*f);
            if (model_engine) model_engine->on_frame(*f);
            if (local_lc) local_lc->log_can_frame(*f);
        });
        socket->start();
    }

    // Show gRPC connection indicator when running as client.
    if (!app.grpc_client.empty()) {
        std::vector<SocketGrpc*> grpc_sockets;
        for (auto& s : sockets)
            if (auto* gs = dynamic_cast<SocketGrpc*>(s.get()))
                grpc_sockets.push_back(gs);
        if (!grpc_sockets.empty()) {
            gui->set_status_indicator(app.grpc_client, [grpc_sockets]() {
                return std::any_of(grpc_sockets.begin(), grpc_sockets.end(),
                                   [](SocketGrpc* s) { return s->connected(); });
            });
        }
    }

    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&gui](const boost::system::error_code& ec, int) {
        if (!ec) gui->stop();
    });

    std::thread asio_thread([&io] { io.run(); });
    gui->run();   // blocks until window closed or stop() called
    io.stop();
    asio_thread.join();
    log_control->stop();
}
