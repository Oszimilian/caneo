#include "compat/print.hpp"
#include "decoder/DecoderRegistry.hpp"
#include "frame/CanFrame.hpp"
#include "frame/DataFrameSet.hpp"
#include "socket/SocketCAN.hpp"

#include <boost/asio.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::println(stderr, "Usage: caneo <interface>[:<dbc>] ...");
        std::println(stderr, "Example: caneo vcan0:vcan0.dbc vcan1");
        return 1;
    }

    boost::asio::io_context io;
    DecoderRegistry decoders;

    std::vector<DataFrameSet> sets;
    std::vector<std::unique_ptr<SocketCAN>> sockets;

    for (const std::string_view arg : std::span(argv + 1, argc - 1)) {
        const auto sep = arg.find(':');
        const std::string iface{arg.substr(0, sep)};

        sets.emplace_back(iface);

        if (sep != std::string_view::npos) {
            decoders.add_interface(iface, std::string{arg.substr(sep + 1)});
        }
    }

    for (std::size_t i = 0; i < sets.size(); ++i) {
        auto& socket = sockets.emplace_back(std::make_unique<SocketCAN>(io, sets[i].interface()));
        socket->onFrame([&sets, &decoders, i](std::unique_ptr<DataFrame> frame) {
            if (auto* canFrame = dynamic_cast<CanFrame*>(frame.get())) {
                try {
                    decoders.decode(*canFrame);
                } catch (const std::runtime_error&) {
                    // no decoder registered for this interface
                }
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
