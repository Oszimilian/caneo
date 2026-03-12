#pragma once

#if __has_include(<print>)
#include <print>
#else
#include <format>
#include <cstdio>

namespace std {

template <typename... Args>
void println(std::format_string<Args...> fmt, Args&&... args) {
    auto s = std::format(fmt, std::forward<Args>(args)...);
    s += '\n';
    fputs(s.c_str(), stdout);
}

template <typename... Args>
void println(FILE* f, std::format_string<Args...> fmt, Args&&... args) {
    auto s = std::format(fmt, std::forward<Args>(args)...);
    s += '\n';
    fputs(s.c_str(), f);
}

} // namespace std
#endif
