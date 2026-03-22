#include "run_live.hpp"

#include "compat/print.hpp"
#include "decoder/DecoderRegistry.hpp"
#include "frame/CanFrame.hpp"
#include "frame/DataFrame.hpp"
#include "frame/DataFrameSet.hpp"
#include "grpc/CanStreamServer.hpp"
#include "logger/LogController.hpp"
#include "model/ModelEngine.hpp"
#include "model/SignalStore.hpp"
#include "proto/ProtoLogRegistry.hpp"
#include "socket/Socket.hpp"
#include "socket/SocketCAN.hpp"
#include "socket/SocketGrpc.hpp"

#include "action/Action.hpp"

#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

void run_live(const AppConfig& app) {
    boost::asio::io_context io;
    DecoderRegistry decoders;
    std::vector<std::unique_ptr<Socket>> sockets;
    std::map<std::string, SocketCAN*>  socket_map;
    std::map<std::string, SocketGrpc*> grpc_socket_map;

    SendFn send_fn = [&socket_map, &grpc_socket_map](const std::string& iface, uint64_t id,
                                                      const std::vector<uint8_t>& data) {
        if (auto it = grpc_socket_map.find(iface); it != grpc_socket_map.end())
            it->second->send(id, data);
        else if (auto it = socket_map.find(iface); it != socket_map.end())
            it->second->send(id, data);
    };

    std::vector<DataFrameSet> sets;
    ProtoLogRegistry proto_registry;
    for (const auto& cfg : app.config.interfaces) {
        sets.emplace_back(cfg.name);
        if (!cfg.dbc.empty()) {
            decoders.add_interface(cfg.name, cfg.dbc);
            proto_registry.add_interface(cfg.name, cfg.dbc);
        }
    }

    LogController log_controller(app, proto_registry);
    if (app.log_mode)
        log_controller.start();

    std::unique_ptr<CanStreamServer> grpc_server;
    if (app.grpc_server) {
        grpc_server = std::make_unique<CanStreamServer>(app.grpc_port);
        grpc_server->set_send_fn(send_fn);
        grpc_server->set_log_controller(&log_controller);
    }

    SignalStore signal_store;
    std::unique_ptr<ModelEngine> model_engine;
    if (!app.model_path.empty())
        model_engine = std::make_unique<ModelEngine>(app.model_path, signal_store, &log_controller);

    for (std::size_t i = 0; i < sets.size(); ++i) {
        std::unique_ptr<Socket> sock;
        if (app.grpc_client.empty()) {
            auto* can = new SocketCAN(io, sets[i].interface());
            socket_map[sets[i].interface()] = can;
            sock.reset(can);
        } else {
            auto* gs = new SocketGrpc(io, sets[i].interface(), app.grpc_client);
            grpc_socket_map[sets[i].interface()] = gs;
            sock.reset(gs);
        }
        auto& socket = sockets.emplace_back(std::move(sock));

        socket->onFrame([&sets, &decoders, &proto_registry, &log_controller,
                         &model_engine, &grpc_server, &app, i]
                        (std::unique_ptr<DataFrame> frame) {
            auto* f = dynamic_cast<CanFrame*>(frame.get());
            if (f) {
                if (grpc_server) grpc_server->broadcast(*f);
                try { decoders.decode(*f); } catch (const std::runtime_error&) {}
                sets[i].update(*f);
                if (model_engine) model_engine->on_frame(*f);
            }
            if (app.debug_mode) {
                for (const auto& set : sets) std::println("{}", set);
                std::println("--------------------------------------------------");
            }
            if (f) {
                if (log_controller.is_logging()) {
                    log_controller.log_can_frame(*f);
                } else if (app.debug_mode) {
                    const std::string desc = proto_registry.describe(*f);
                    if (!desc.empty()) std::println("proto:\n{}", desc);
                }
            }
        });
        socket->start();
        std::println("Listening on [{}]...", sets[i].interface());
    }

    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&io, &log_controller](const boost::system::error_code&, int) {
        log_controller.stop();
        io.stop();
    });

    io.run();
}
