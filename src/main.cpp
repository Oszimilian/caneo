#include "frame/CanFrame.hpp"
#include "frame/DataFrameSet.hpp"
#include "socket/SocketCAN.hpp"

#include <boost/asio.hpp>
#include <memory>
#include <print>
#include <span>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::println(stderr, "Usage: caneo <interface> [interface...]");
        std::println(stderr, "Example: caneo vcan0 vcan1");
        return 1;
    }

    boost::asio::io_context io;

    std::vector<DataFrameSet> sets;
    std::vector<std::unique_ptr<SocketCAN>> sockets;

    for (const auto& iface : std::span(argv + 1, argc - 1)) {
        sets.emplace_back(iface);
    }

    for (std::size_t i = 0; i < sets.size(); ++i) {
        auto& socket = sockets.emplace_back(std::make_unique<SocketCAN>(io, sets[i].interface()));
        socket->onFrame([&sets, i](std::unique_ptr<DataFrame> frame) {
            if (auto* canFrame = dynamic_cast<CanFrame*>(frame.get())) {
                sets[i].update(*canFrame);
            }

            for (const auto& set : sets) {
                std::println("{}", set);
            }
            std::println("--------------------------------------------------");
        });
        socket->start();
        std::println("Listening on {}...", *socket);
    }

    io.run();
}
