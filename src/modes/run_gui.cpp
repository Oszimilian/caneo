#include "run_gui.hpp"
#include "log_frame.hpp"

#include "action/ActionHandler.hpp"
#include "action/ActionPreset.hpp"
#include "gui/GraphPreset.hpp"
#include "gui/ModelWindow.hpp"
#include "decoder/DecoderRegistry.hpp"
#include "frame/CanFrame.hpp"
#include "gui/Gui.hpp"
#include "logger/McapLogger.hpp"
#include "model/ModelEngine.hpp"
#include "model/SignalStore.hpp"
#include "proto/ProtoLogRegistry.hpp"
#include "socket/SocketCAN.hpp"

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
    std::vector<std::unique_ptr<SocketCAN>> sockets;
    std::map<std::string, SocketCAN*>       socket_map;

    SendFn send_fn = [&socket_map](const std::string& iface, uint64_t id,
                                   const std::vector<uint8_t>& data) {
        if (auto it = socket_map.find(iface); it != socket_map.end())
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
    std::unique_ptr<Logger> logger;
    if (app.log_mode) {
        for (const auto& cfg : app.config.interfaces)
            if (!cfg.dbc.empty())
                proto_registry.add_interface(cfg.name, cfg.dbc);
        logger = std::make_unique<McapLogger>(app.make_log_path());
    }

    FrameTimestampMap last_frame_ts;
    SignalStore       signal_store;
    std::unique_ptr<ModelEngine> model_engine;
    if (!app.model_path.empty()) {
        model_engine = std::make_unique<ModelEngine>(app.model_path, signal_store, logger.get());
        auto* mw = gui->add_model_window();
        model_engine->set_output_callback([mw](const std::string& name, double value) {
            mw->push(name, value);
        });
    }

    for (const auto& cfg : app.config.interfaces) {
        if (!cfg.dbc.empty())
            decoders.add_interface(cfg.name, cfg.dbc);
        auto& socket = sockets.emplace_back(std::make_unique<SocketCAN>(io, cfg.name));
        socket_map[cfg.name] = socket.get();
        socket->onFrame([gui, &decoders, &proto_registry, &logger, &last_frame_ts, &model_engine]
                        (std::unique_ptr<DataFrame> frame) {
            auto* f = dynamic_cast<CanFrame*>(frame.get());
            if (!f) return;
            try { decoders.decode(*f); } catch (const std::runtime_error&) {}
            gui->update(*f);
            if (model_engine) model_engine->on_frame(*f);
            log_frame(*f, logger.get(), proto_registry, last_frame_ts);
        });
        socket->start();
    }

    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&gui](const boost::system::error_code& ec, int) {
        if (!ec) gui->stop();
    });

    std::thread asio_thread([&io] { io.run(); });
    gui->run();   // blocks until window closed or stop() called
    io.stop();
    asio_thread.join();
    logger.reset();
}
