#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

// Read-only description of everything a DBC file defines, for the DBC viewer.
struct DbcInfo {
    struct Signal {
        std::string name, unit, comment, receivers, mux;
        uint64_t    start_bit = 0, bit_size = 0;
        bool        little_endian = true, is_signed = false;
        double      factor = 1.0, offset = 0.0, min = 0.0, max = 0.0;
        std::vector<std::string> values;   // "3 = Drive"
    };
    struct Message {
        uint32_t    id = 0;              // without the extended flag
        bool        extended = false;
        std::string name, sender, comment;
        uint64_t    size = 0;
        std::vector<Signal> signals;
    };

    std::string          path;
    std::vector<Message> messages;   // sorted by id

    // Throws std::runtime_error if the file cannot be read or parsed.
    static DbcInfo load(const std::filesystem::path& path);

    friend std::ostream& operator<<(std::ostream& os, const DbcInfo& info);
};

template <>
struct std::formatter<DbcInfo> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const DbcInfo& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
