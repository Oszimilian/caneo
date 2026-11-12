#include "config_picker.hpp"

#include <algorithm>
#include <filesystem>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <vector>

std::optional<std::string> pick_config_tui()
{
    namespace fs = std::filesystem;
    using namespace ftxui;

    fs::path                 dir = fs::current_path();
    std::vector<std::string> names;
    std::vector<bool>        is_dir;
    int                      sel = 0;
    std::optional<std::string> result;

    auto refresh = [&] {
        names.clear();
        is_dir.clear();
        std::error_code ec;
        if (dir.has_parent_path() && dir != dir.root_path()) {
            names.push_back("../");
            is_dir.push_back(true);
        }
        std::vector<std::pair<std::string, bool>> items;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string n = e.path().filename().string();
            if (n.starts_with(".")) continue;
            const bool d = e.is_directory(ec);
            const auto ext = e.path().extension().string();
            if (d || ext == ".yaml" || ext == ".yml") items.emplace_back(n, d);
        }
        std::ranges::sort(items, [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        for (const auto& [n, d] : items) {
            names.push_back(d ? n + "/" : n);
            is_dir.push_back(d);
        }
        sel = 0;
    };
    refresh();

    auto screen = ScreenInteractive::Fullscreen();
    auto activate = [&] {
        if (sel < 0 || sel >= static_cast<int>(names.size())) return;
        const std::string n = names[static_cast<std::size_t>(sel)];
        if (n == "../") {
            dir = dir.parent_path();
            refresh();
        } else if (is_dir[static_cast<std::size_t>(sel)]) {
            dir /= n.substr(0, n.size() - 1);
            refresh();
        } else {
            result = (dir / n).string();
            screen.ExitLoopClosure()();
        }
    };

    MenuOption opt;
    opt.on_enter = activate;
    auto menu = Menu(&names, &sel, opt);
    auto view = Renderer(menu, [&] {
        return vbox({
                   text("caneo - no caneo.yaml in the working directory") | bold,
                   text("Search a config file:  Up/Down select, Enter open / pick, Backspace parent directory, Esc quit") | dim,
                   text(dir.string()),
                   separator(),
                   names.empty() ? text("(no directories or .yaml files here)") | dim
                                 : menu->Render() | vscroll_indicator | yframe | flex,
               }) | border;
    });
    view |= CatchEvent([&](Event e) {
        if (e == Event::Escape || e == Event::Character('q')) {
            screen.ExitLoopClosure()();
            return true;
        }
        if (e == Event::Backspace && dir.has_parent_path() && dir != dir.root_path()) {
            dir = dir.parent_path();
            refresh();
            return true;
        }
        return false;
    });
    screen.Loop(view);
    return result;
}
