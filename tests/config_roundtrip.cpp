// Loads a caneo config (legacy or current layout), saves it, loads the saved
// file again and checks that nothing was lost.
//
//   caneo_config_test <config.yaml> <output.yaml>
#include "config/Config.hpp"

#include <cstdio>
#include <format>
#include <string>

static int failures = 0;

static void check(bool ok, const std::string& what)
{
    if (!ok) {
        std::fputs(std::format("FAIL: {}\n", what).c_str(), stderr);
        ++failures;
    }
}

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::fputs("usage: caneo_config_test <config.yaml> <output.yaml>\n", stderr);
        return 2;
    }
    const Config a = load_config(argv[1]);
    save_config(a, argv[2]);
    const Config b = load_config(argv[2]);

    check(a.virtual_can == b.virtual_can, "virtual");
    check(a.log_file_path == b.log_file_path, "log_file_path");
    check(a.log_viewer == b.log_viewer, "log_viewer");
    check(a.gui == b.gui, "gui");
    check(a.logging == b.logging, "logging");
    check(a.same_channels(b), "channels");
    check(a.graphs.size() == b.graphs.size(), "graph count");
    check(a.guards == b.guards, "guards");
    for (std::size_t i = 0; i < a.interfaces.size() && i < b.interfaces.size(); ++i)
        check(a.interfaces[i].actions == b.interfaces[i].actions,
              std::format("actions of {}", a.interfaces[i].name));
    for (std::size_t i = 0; i < a.graphs.size() && i < b.graphs.size(); ++i) {
        check(a.graphs[i].name == b.graphs[i].name, "graph name");
        check(a.graphs[i].signals.size() == b.graphs[i].signals.size(), "graph signals");
    }

    std::size_t n_actions = 0;
    for (const auto& i : a.interfaces) n_actions += i.actions.size();
    std::fputs(std::format("{}: {} interfaces, {} actions, {} graphs, {} guards -> {}\n",
                           argv[1], a.interfaces.size(), n_actions, a.graphs.size(), a.guards.size(),
                           failures ? "FAILED" : "OK").c_str(), stdout);
    return failures ? 1 : 0;
}
