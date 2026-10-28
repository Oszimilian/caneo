#include "LogViewer.hpp"

#include "config/Config.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace log_viewer {

static std::string shell_quote(const std::string& s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    return out + "'";
}

std::optional<std::string> launch(const std::string& command, const std::string& file)
{
    if (command.empty())
        return "No viewer command configured (Settings > Logging).";

    std::string cmd = command;
    const std::string quoted = file.empty() ? std::string{} : shell_quote(fs::absolute(file).string());
    if (const auto pos = cmd.find("{file}"); pos != std::string::npos) {
        if (file.empty()) {
            // No file: drop the placeholder and a directly preceding option
            // such as "-d " so the tool still starts.
            std::size_t start = pos;
            while (start > 0 && cmd[start - 1] == ' ') --start;
            const std::size_t opt = cmd.rfind(' ', start == 0 ? 0 : start - 1);
            if (opt != std::string::npos && cmd[opt + 1] == '-')
                start = opt;
            cmd.erase(start, pos + 6 - start);
        } else {
            cmd.replace(pos, 6, quoted);
        }
    } else if (!file.empty()) {
        cmd += " " + quoted;
    }

    // Detached: the GUI must not wait for (or be killed with) the viewer.
    const std::string full = std::format("setsid sh -c {} >/dev/null 2>&1 &", shell_quote(cmd));
    if (std::system(full.c_str()) != 0)
        return "Could not start: " + cmd;
    return std::nullopt;
}

std::optional<std::string> detect_plotjuggler()
{
    std::vector<fs::path> found;
    std::error_code ec;

    const char* home = std::getenv("HOME");
    if (home) {
        for (const char* sub : {"Downloads", "Applications", "bin", ".local/bin"}) {
            const fs::path dir = fs::path(home) / sub;
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                std::string name = e.path().filename().string();
                std::string low  = name;
                std::ranges::transform(low, low.begin(), [](unsigned char c) { return std::tolower(c); });
                if (low.find("plotjuggler") != std::string::npos && e.is_regular_file(ec))
                    found.push_back(e.path());
            }
        }
    }
    if (const char* path = std::getenv("PATH")) {
        std::stringstream ss(path);
        std::string dir;
        while (std::getline(ss, dir, ':'))
            if (fs::exists(fs::path(dir) / "plotjuggler", ec))
                found.push_back(fs::path(dir) / "plotjuggler");
    }
    if (found.empty())
        return std::nullopt;

    // Highest version first: file names like PlotJuggler-3.17.2-x86_64.AppImage
    // sort correctly as long as the major versions have the same width.
    std::ranges::sort(found, std::greater<>{});
    std::string p = found.front().string();
    if (home && p.starts_with(std::string(home) + "/"))
        p = "~/" + p.substr(std::string(home).size() + 1);
    return p + " -n -d {file}";
}

} // namespace log_viewer
