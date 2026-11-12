#include "RecentConfigs.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

fs::path store_path()
{
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return fs::path(x) / "caneo" / "recent_configs";
    if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h) / ".config" / "caneo" / "recent_configs";
    return {};
}

std::vector<std::string> read_all()
{
    std::vector<std::string> out;
    const fs::path p = store_path();
    if (p.empty()) return out;
    std::ifstream f(p);
    for (std::string line; std::getline(f, line);)
        if (!line.empty()) out.push_back(line);
    return out;
}

void write_all(const std::vector<std::string>& list)
{
    const fs::path p = store_path();
    if (p.empty()) return;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::trunc);
    for (const auto& s : list) f << s << '\n';
}

}   // namespace

std::vector<std::string> recent_configs()
{
    auto list = read_all();
    std::error_code ec;
    std::erase_if(list, [&](const std::string& s) { return !fs::exists(s, ec); });
    return list;
}

void remember_config(const std::string& path)
{
    std::error_code ec;
    const std::string abs = fs::absolute(path, ec).lexically_normal().string();
    auto list = read_all();
    std::erase(list, abs);
    list.insert(list.begin(), abs);
    if (list.size() > 20) list.resize(20);
    write_all(list);
}

void forget_config(const std::string& path)
{
    auto list = read_all();
    std::erase(list, path);
    write_all(list);
}
