#include "run_gui.hpp"

#include "compat/print.hpp"
#include "config/RecentConfigs.hpp"
#include "gui/AppState.hpp"
#include "gui/FlowEditorWindow.hpp"
#include "flow/FlowEngine.hpp"
#include "flow/Legacy.hpp"
#include "gui/GraphPreset.hpp"
#include "gui/ModelWindow.hpp"
#include "decoder/DecoderRegistry.hpp"
#include "frame/CanFrame.hpp"
#include "gui/Gui.hpp"
#include "logger/ILogControl.hpp"
#include "logger/LogController.hpp"
#include "grpc/RemoteLogControl.hpp"
#include "model/ModelEngine.hpp"
#include "send/ModelCache.hpp"
#include "model/SignalStore.hpp"
#include "proto/ProtoLogRegistry.hpp"
#include "grpc/CanStreamServer.hpp"
#include "grpc/RemoteInterfaces.hpp"
#include "setup/InterfaceSetup.hpp"
#include "socket/Socket.hpp"
#include "socket/SocketCAN.hpp"
#include "socket/SocketGrpc.hpp"
#include <algorithm>

#include <boost/asio.hpp>
#include <csignal>
#include <future>
#include <mutex>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

bool run_gui(AppConfig& app)
{
    boost::asio::io_context io;
    DecoderRegistry         decoders;
    std::vector<std::unique_ptr<Socket>> sockets;
    std::map<std::string, SocketCAN*>  socket_map;       // populated in SocketCAN mode
    std::map<std::string, SocketGrpc*> grpc_socket_map;  // populated in grpc_client mode
    std::mutex                         grpc_map_mutex;   // the render thread reads connected()

    SendFn send_fn = [&socket_map, &grpc_socket_map](const std::string& iface, uint64_t id,
                                                      const std::vector<uint8_t>& data) {
        if (auto it = grpc_socket_map.find(iface); it != grpc_socket_map.end())
            it->second->send(id, data);
        else if (auto it = socket_map.find(iface); it != socket_map.end())
            it->second->send(id, data);
    };

    // saved actions / guards run as flows in the GUI
    const auto converted = migrate_legacy(app.config);

    // Latest value of every received signal; read by Lua blocks and the model.
    // Only accessed on the asio thread.
    SignalStore   signal_store;
    ModelCache    model_cache;
    model_cache.set_interfaces(app.config.interfaces);

    FlowEngine    flow_engine(io, &signal_store, send_fn);
    flow_engine.set_model_fn([&model_cache](const std::string& iface) { return model_cache.model_for(iface); });

    AppState app_state(app.config, app.config_path, model_cache, flow_engine);
    // the Send window sends from the GUI thread: hand the frame to the asio thread
    app_state.set_send_fn([&io, send_fn](const std::string& iface, uint64_t id, const std::vector<uint8_t>& data) {
        boost::asio::post(io, [send_fn, iface, id, data] { send_fn(iface, id, data); });
    });

    if (!converted.empty())
        app_state.set_status(std::format("{} action(s)/guard(s) of the config now run as flows - save to keep them", converted.size()));

    auto gui = std::make_shared<Gui>(app.config.interfaces, app_state);

    apply_graph_config(app.config, *gui->graph_window(),
                       [&model_cache](const std::string& iface) { return model_cache.model_for(iface); });

    if (!app.config_found)
        gui->search_config();
    else if (std::filesystem::exists(app.config_path))
        remember_config(app.config_path);

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
    // XCP measurements go into the local log as xcp/<variable>
    if (local_lc)
        app_state.set_scalar_log([local_lc](const std::string& topic, double v) { local_lc->log_scalar(topic, v); });

    // Scope blocks of the flows: plotted like guard outputs and logged.
    flow_engine.set_output_fn([gui, local_lc](const std::string& flow, const std::string& scope, double v) {
        gui->graph_window()->push_value(GraphWindow::GUARD_IFACE, FlowEditorWindow::series_name(flow, scope), v);
        if (local_lc) local_lc->log_scalar(FlowEditorWindow::series_name(flow, scope), v);
    });
    flow_engine.add_all(app.config.flows);

    std::unique_ptr<CanStreamServer> grpc_server;
    if (app.grpc_server) {
        grpc_server = std::make_unique<CanStreamServer>(app.grpc_port);
        grpc_server->set_send_fn(send_fn);
        grpc_server->set_interfaces(app.config.interfaces);
    }

    std::unique_ptr<ModelEngine> model_engine;
    if (!app.model_path.empty()) {
        model_engine = std::make_unique<ModelEngine>(app.model_path, signal_store, local_lc);
        auto* mw = gui->add_model_window();
        model_engine->set_output_callback([mw](const std::string& name, double value) {
            mw->push(name, value);
        });
    }

    // Decoders need no privileges: build them right away.
    std::vector<std::string> socket_errors;
    for (const auto& cfg : app.config.interfaces) {
        if (cfg.dbc.empty()) continue;
        try {
            decoders.add_interface(cfg.name, expand_path(cfg.dbc).string());
        } catch (const std::exception& e) {
            std::println(stderr, "Warning: {}", e.what());
            socket_errors.push_back(std::format("{}: {}", cfg.name, e.what()));
        }
    }

    // Opens the sockets of all running channels. Only on the asio thread (socket_map is read there).
    auto open_sockets = [&]() -> std::vector<std::string> {
        std::vector<std::string> errors;
        for (const auto& cfg : app.config.interfaces) {
            std::unique_ptr<Socket> sock;
            if (app.grpc_client.empty() && !cfg.is_remote()) {
                // A missing interface must not kill the GUI.
                SocketCAN* can = nullptr;
                try {
                    can = new SocketCAN(io, cfg.name);
                } catch (const std::exception& e) {
                    std::println(stderr, "Warning: {}", e.what());
                    errors.push_back(e.what());
                    continue;
                }
                socket_map[cfg.name] = can;
                sock.reset(can);
            } else {
                auto* gs = cfg.is_remote() ? new SocketGrpc(io, cfg.name, cfg.grpc, cfg.remote_name())
                                          : new SocketGrpc(io, cfg.name, app.grpc_client);
                {
                    std::lock_guard lock(grpc_map_mutex);
                    grpc_socket_map[cfg.name] = gs;
                }
                sock.reset(gs);
            }
            auto& socket = sockets.emplace_back(std::move(sock));
            socket->onFrame([gui, &decoders, local_lc, &model_engine, &grpc_server, &signal_store,
                             live = cfg.live, to_log = cfg.log]
                            (std::unique_ptr<DataFrame> frame) {
                auto* f = dynamic_cast<CanFrame*>(frame.get());
                if (!f) return;
                if (grpc_server) grpc_server->broadcast(*f);
                try { decoders.decode(*f); } catch (const std::runtime_error&) {}
                if (live) gui->update(*f);
                if (model_engine) model_engine->on_frame(*f);   // updates signal_store too
                else              signal_store.update(*f);
                if (local_lc && to_log) local_lc->log_can_frame(*f);
            });
            socket->start();
        }
        return errors;
    };
    auto close_sockets = [&]() -> std::vector<std::string> {
        for (auto& s : sockets) s->stop();
        socket_map.clear();
        {
            std::lock_guard lock(grpc_map_mutex);
            grpc_socket_map.clear();
        }
        sockets.clear();
        return {};
    };
    // Runs fn on the asio thread and waits for it.
    auto on_io = [&io](auto fn) {
        std::promise<std::vector<std::string>> done;
        auto result = done.get_future();
        boost::asio::post(io, [&] {
            try { done.set_value(fn()); } catch (...) { done.set_exception(std::current_exception()); }
        });
        return result.get();
    };

    // "Start": set the interfaces up (one password prompt, only if something has to change), open the sockets.
    auto start_channels = [&](bool graphical) -> Gui::ChannelResult {
        if (app.grpc_client.empty()) {
            std::vector<std::string> warnings;
            const auto cmds = plan_setup(app.config, &warnings);
            if (const auto err = run_privileged(cmds, graphical); !err.empty())
                return {false, "Interface setup failed: " + err};
            for (const auto& w : warnings) std::println(stderr, "Warning: {}", w);
        }
        auto errors = on_io(open_sockets);
        if (app.config.interfaces.empty())
            return {false, "No channels: connect devices to a Live or Log block in the Inputs window"};
        const bool any = !sockets.empty();
        if (errors.empty())
            return {any, any ? "Channels started" : "No channel could be opened"};
        std::string msg = "Problems with the configured channels:";
        for (const auto& e : errors) msg += "\n  " + e;
        return {any, msg};
    };
    gui->set_channel_control(start_channels, [&] { on_io(close_sockets); });

    // "Reset HW": local channels down + up (password only if caneo lacks CAP_NET_ADMIN), remote ones on their server
    gui->set_reset_control([&app](bool graphical) -> std::string {
        std::vector<std::string>                        local;
        std::map<std::string, std::vector<std::string>> remote;   // server address -> channel names there
        for (const auto& cfg : app.config.interfaces) {
            if (cfg.is_remote())                remote[cfg.grpc].push_back(cfg.remote_name());
            else if (!app.grpc_client.empty())  remote[app.grpc_client].push_back(cfg.name);
            else                                local.push_back(cfg.name);
        }
        std::string err;
        const auto add = [&err](const std::string& e) { if (!e.empty()) err += (err.empty() ? "" : "\n  ") + e; };
        if (!local.empty()) add(reset_interfaces(local, /*allow_privileged=*/true, graphical));
        for (const auto& [address, names] : remote)
            if (auto r = reset_remote_interfaces(address, names); !r) add(address + ": " + r.error());
        return err;
    });

    if (!socket_errors.empty()) {
        std::string msg = "Problems with the configured channels:";
        for (const auto& e : socket_errors) msg += "\n  " + e;
        app_state.set_status(msg, true);
    }

    // Show gRPC connection indicator when running as client or with remote channels (gRPC SocketCAN blocks).
    std::string grpc_label = app.grpc_client;
    for (const auto& cfg : app.config.interfaces)
        if (cfg.is_remote() && grpc_label.find(cfg.grpc) == std::string::npos)
            grpc_label += (grpc_label.empty() ? "" : ", ") + cfg.grpc;
    if (!grpc_label.empty()) {
        // read from the render thread while the asio thread may change the map: ask the asio thread
        gui->set_status_indicator(grpc_label, [&]() {
            bool connected = false;
            std::lock_guard lock(grpc_map_mutex);
            for (auto& [name, gs] : grpc_socket_map) connected = connected || gs->connected();
            return connected;
        });
    }

    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&gui](const boost::system::error_code& ec, int) {
        if (!ec) gui->stop();
    });

    // Keep io.run() alive even when no async reads are pending (gRPC client,
    // or no interfaces at all).
    auto work_guard = boost::asio::make_work_guard(io);
    std::thread asio_thread([&io] { io.run(); });
    if (app.log_mode && !app.config.interfaces.empty()) {   // --log: nothing to click, start right away
        const auto res = gui->start_channels_now(false);
        app_state.set_status(res.message, !res.running);
    }
    gui->run();   // blocks until window closed, stop() or a restart request
    work_guard.reset();
    io.stop();
    asio_thread.join();
    log_control->stop();

    app.config_path = app_state.path();
    return app_state.restart_requested();
}
