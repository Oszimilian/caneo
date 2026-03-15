#include "run_playback.hpp"

#include "action/ActionHandler.hpp"
#include "compat/print.hpp"
#include "gui/TuiDataFrameSet.hpp"
#include "logger/McapReader.hpp"
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

    McapReader::OnSend on_send = [&socket_map](const std::string& iface,
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

        std::vector<std::string> iface_names;
        for (const auto& cfg : app.config.interfaces)
            iface_names.push_back(cfg.name);

        PlaybackController playback_ctrl(iface_names);
        for (const auto& [iface, msgs] : McapReader(app.playback_path).scan_messages())
            playback_ctrl.set_messages(iface, msgs);

        auto tui = std::make_shared<TuiDataFrameSet>(
            app.config.interfaces, action_handler, &playback_ctrl);

        std::thread playback_thread([&app, tui, &on_send, &playback_ctrl] {
            try {
                McapReader reader(app.playback_path);
                reader.play(
                    [&tui](std::unique_ptr<CanFrame> frame) { tui->update(*frame); },
                    on_send,
                    &playback_ctrl);
            } catch (const std::exception& e) {
                std::println(stderr, "Playback error: {}", e.what());
            }
        });

        std::thread asio_thread([&io] { io.run(); });
        tui->run();
        playback_ctrl.stop();
        io.stop();
        asio_thread.join();
        playback_thread.join();
    } else {
        std::thread asio_thread([&io] { io.run(); });
        try {
            McapReader(app.playback_path).play({}, on_send);
        } catch (const std::exception& e) {
            std::println(stderr, "Playback error: {}", e.what());
        }
        io.stop();
        asio_thread.join();
    }
}
