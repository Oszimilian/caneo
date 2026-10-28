#include "run_playback.hpp"

#include "action/ActionHandler.hpp"
#include "compat/print.hpp"
#include "gui/TuiDataFrameSet.hpp"
#include "config/Config.hpp"
#include "decoder/DecoderRegistry.hpp"
#include "guard/GuardEngine.hpp"
#include "model/SignalStore.hpp"
#include "logger/BlfReader.hpp"
#include "logger/LogReader.hpp"
#include "playback/PlaybackController.hpp"
#include "socket/SocketCAN.hpp"

#include <boost/asio.hpp>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

void run_playback(const AppConfig& app) {
    boost::asio::io_context io;
    std::vector<std::unique_ptr<SocketCAN>> sockets;
    std::map<std::string, SocketCAN*> socket_map;

    for (const auto& cfg : app.config.interfaces) {
        try {
            auto& socket = sockets.emplace_back(std::make_unique<SocketCAN>(io, cfg.name));
            socket_map[cfg.name] = socket.get();
        } catch (const std::exception& e) {
            std::println(stderr, "Warning: cannot open {}: {}", cfg.name, e.what());
        }
    }

    // MCAP or BLF (detected from the file). BLF only stores raw frames: decode
    // them with the DBCs of the config so the TUI shows signals.
    std::unique_ptr<LogReader> reader = open_log_reader(app.playback_path);
    DecoderRegistry decoders;
    std::vector<std::string> iface_names;
    for (const auto& cfg : app.config.interfaces) {
        iface_names.push_back(cfg.name);
        if (!cfg.dbc.empty()) {
            try { decoders.add_interface(cfg.name, expand_path(cfg.dbc).string()); }
            catch (const std::exception& e) { std::println(stderr, "Warning: {}", e.what()); }
        }
    }
    reader->set_decoder([&decoders](CanFrame& f) { decoders.decode(f); });
    if (auto* blf = dynamic_cast<BlfReader*>(reader.get()))
        blf->set_channel_names(iface_names);

    LogReader::OnSend on_send = [&socket_map](const std::string& iface,
                                               uint32_t can_id,
                                               const std::vector<uint8_t>& payload) {
        if (auto it = socket_map.find(iface); it != socket_map.end())
            it->second->send(can_id, payload);
    };

    if (app.tui_mode) {
        SendFn send_fn = [&socket_map](const std::string& iface, uint64_t id,
                                       const std::vector<uint8_t>& data) {
            if (auto it = socket_map.find(iface); it != socket_map.end())
                it->second->send(id, data);
        };
        ActionHandler action_handler(io, send_fn);

        PlaybackController playback_ctrl(iface_names);
        for (const auto& [iface, msgs] : reader->scan_messages())
            playback_ctrl.set_messages(iface, msgs);

        auto tui = std::make_shared<TuiDataFrameSet>(
            app.config.interfaces, action_handler, &playback_ctrl);

        // Guards evaluate the replayed frames (on the asio thread, like live)
        SignalStore signal_store;
        GuardEngine guard_engine(io, &signal_store);
        guard_engine.set_output_fn([tui](const std::string& guard, const std::string& output, double v) {
            tui->push_guard_output(guard, output, v);
        });
        guard_engine.add_all(app.config.guards);
        tui->set_guard_engine(&guard_engine);
        tui->set_graphs(app.config.graphs);

        std::thread playback_thread([&reader, tui, &on_send, &playback_ctrl, &io, &signal_store, &guard_engine] {
            try {
                reader->play(
                    [&tui, &io, &signal_store, &guard_engine](std::unique_ptr<CanFrame> frame) {
                        tui->update(*frame);
                        boost::asio::post(io, [&signal_store, &guard_engine,
                                               f = std::shared_ptr<CanFrame>(std::move(frame))] {
                            signal_store.update(*f);
                            guard_engine.on_frame(*f);
                        });
                    },
                    on_send,
                    &playback_ctrl);
            } catch (const std::exception& e) {
                std::println(stderr, "Playback error: {}", e.what());
            }
        });

        auto work_guard = boost::asio::make_work_guard(io);   // posted guard work
        std::thread asio_thread([&io] { io.run(); });
        tui->run();
        work_guard.reset();
        playback_ctrl.stop();
        io.stop();
        asio_thread.join();
        playback_thread.join();
    } else {
        std::thread asio_thread([&io] { io.run(); });
        try {
            reader->play({}, on_send);
        } catch (const std::exception& e) {
            std::println(stderr, "Playback error: {}", e.what());
        }
        io.stop();
        asio_thread.join();
    }
}
