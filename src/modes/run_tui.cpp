#include "run_tui.hpp"
#include "log_frame.hpp"

#include "action/ActionHandler.hpp"
#include "compat/print.hpp"
#include "decoder/DecoderRegistry.hpp"
#include "frame/CanFrame.hpp"
#include "frame/DataFrame.hpp"
#include "gui/TuiDataFrameSet.hpp"
#include "logger/McapLogger.hpp"
#include "model/ModelEngine.hpp"
#include "model/SignalStore.hpp"
#include "proto/ProtoLogRegistry.hpp"
#include "socket/SocketCAN.hpp"

#include <boost/asio.hpp>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

void run_tui(const AppConfig& app) {
    boost::asio::io_context io;
    DecoderRegistry decoders;
    std::vector<std::unique_ptr<SocketCAN>> sockets;
    std::map<std::string, SocketCAN*> socket_map;

    SendFn send_fn = [&socket_map](const std::string& iface, uint64_t id,
                                   const std::vector<uint8_t>& data) {
        if (auto it = socket_map.find(iface); it != socket_map.end())
            it->second->send(id, data);
    };

    ActionHandler action_handler(io, send_fn);
    auto tui = std::make_shared<TuiDataFrameSet>(app.config.interfaces, action_handler);

    ProtoLogRegistry proto_registry;
    std::unique_ptr<Logger> logger;
    if (app.log_mode) {
        for (const auto& cfg : app.config.interfaces)
            if (!cfg.dbc.empty())
                proto_registry.add_interface(cfg.name, cfg.dbc);
        logger = std::make_unique<McapLogger>(app.make_log_path());
    }

    FrameTimestampMap last_frame_ts;
    SignalStore signal_store;
    std::unique_ptr<ModelEngine> model_engine;
    if (!app.model_path.empty())
        model_engine = std::make_unique<ModelEngine>(app.model_path, signal_store, logger.get());

    for (const auto& cfg : app.config.interfaces) {
        if (!cfg.dbc.empty())
            decoders.add_interface(cfg.name, cfg.dbc);
        auto& socket = sockets.emplace_back(std::make_unique<SocketCAN>(io, cfg.name));
        socket_map[cfg.name] = socket.get();
        socket->onFrame([tui, &decoders, &proto_registry, &logger, &last_frame_ts, &model_engine]
                        (std::unique_ptr<DataFrame> frame) {
            auto* f = dynamic_cast<CanFrame*>(frame.get());
            if (!f) return;
            try { decoders.decode(*f); } catch (const std::runtime_error&) {}
            tui->update(*f);
            if (model_engine) model_engine->on_frame(*f);
            log_frame(*f, logger.get(), proto_registry, last_frame_ts);
        });
        socket->start();
    }

    std::thread asio_thread([&io] { io.run(); });
    tui->run();
    io.stop();
    asio_thread.join();
    logger.reset();
}
