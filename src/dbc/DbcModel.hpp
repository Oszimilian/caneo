#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Editable in-memory DBC (the "DBC" mode). Independent of dbcppp: a small own parser / writer
// for BU_, BO_, SG_, CM_, VAL_ and VERSION. BA_* lines are kept verbatim.
struct DbcSignal {
    std::string name;
    int         start_bit = 0;
    int         bit_size  = 8;
    bool        little_endian = true;   // Intel (@1)
    bool        is_signed     = false;
    int         float_type    = 0;      // SIG_VALTYPE_: 0 integer, 1 IEEE float (32 bit), 2 IEEE double (64 bit)
    double      factor = 1.0, offset = 0.0, min = 0.0, max = 0.0;
    std::string unit;
    std::vector<std::string> receivers;     // node names
    std::string mux;                        // "", "M", "m3", "m3M"
    std::string comment;
    std::vector<std::pair<int64_t, std::string>> values;   // value table

    // Bit positions (byte * 8 + bit, bit 0 = LSB) in the order MSB..LSB (Motorola) / LSB..MSB (Intel).
    std::vector<int> bits() const;
};

struct DbcMessage {
    uint32_t    id = 0;              // without the extended flag
    bool        extended = false;
    uint32_t    high_bits = 0;       // bits 29/30 of the raw ID (e.g. Vector's 0xC0000000 pseudo message), kept as read
    std::string name;
    int         size = 8;            // bytes
    std::string sender;              // node name, empty = none
    std::string comment;
    std::vector<DbcSignal> signals;

    // Per bit (byte * 8 + bit): index of the signal occupying it, -1 free, -2 overlap.
    std::vector<int> layout() const;
};

struct DbcNode {
    std::string name;
    std::string comment;
};

struct DbcDoc {
    std::string             version;
    std::vector<DbcNode>    nodes;
    std::vector<DbcMessage> messages;
    std::vector<std::string> extra;   // BA_DEF_ / BA_DEF_DEF_ / BA_ lines, written back unchanged

    // Throws std::runtime_error on I/O errors.
    static DbcDoc load(const std::filesystem::path& path);
    static DbcDoc parse(const std::string& text);
    std::string   to_string() const;
    void          save(const std::filesystem::path& path) const;

    int  find_node(const std::string& name) const;     // -1 if unknown
    void rename_node(std::size_t idx, const std::string& new_name);   // updates senders / receivers
    void remove_node(std::size_t idx);                  // clears senders / receivers

    // Human readable problems (duplicates, overlaps, out of range ...).
    std::vector<std::string> problems() const;

    friend std::ostream& operator<<(std::ostream& os, const DbcDoc& doc);
};

template <>
struct std::formatter<DbcDoc> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const DbcDoc& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
