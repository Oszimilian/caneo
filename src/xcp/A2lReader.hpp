#pragma once

#include "xcp/ElfReader.hpp"

#include <cstdint>
#include <format>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// One MEASUREMENT / CHARACTERISTIC of an A2L (ASAM MCD-2 MC) file.
struct A2lObject {
    std::string           name;          // A2L name = C expression for Embedded Coder ("model_B.Saturation")
    std::string           long_id;       // LONG_IDENTIFIER
    bool                  characteristic = false;
    std::string           kind;          // MEASUREMENT, VALUE, VAL_BLK, CURVE, MAP, ASCII, ...
    std::string           data_type;     // UBYTE, FLOAT64_IEEE, ...
    XcpType               type = XcpType::None;
    uint64_t              address = 0;   // ECU_ADDRESS (0 = not filled in)
    std::vector<uint32_t> dims;          // MATRIX_DIM / ARRAY_SIZE / NUMBER / axis points; empty = scalar
    uint64_t              bit_mask = 0;  // 0 = none
    XcpConversion         conv;
    std::vector<std::pair<int64_t, std::string>> enumerators;   // COMPU_VTAB
    std::string           unit;
    double                lower = 0.0, upper = 0.0;
    bool                  read_only = false;
    std::string           symbol;        // SYMBOL_LINK
    int64_t               symbol_offset = 0;
};

struct A2lModule {
    std::string              path;
    std::string              name;          // MODULE name
    std::optional<bool>      little_endian; // MOD_COMMON BYTE_ORDER
    std::vector<A2lObject>   objects;
    std::vector<std::string> warnings;
};

// Parses MEASUREMENT, CHARACTERISTIC, COMPU_METHOD, COMPU_VTAB(_RANGE), RECORD_LAYOUT, UNIT and MOD_COMMON.
// Throws std::runtime_error (file missing, syntax error).
A2lModule load_a2l(const std::string& path);
A2lModule parse_a2l(const std::string& text, std::string path = {});

// The variable tree of an A2L, usable like an ELF image (leaf path = A2L name). The tree follows the
// LONG_IDENTIFIER when it is a path ("model/Sine Wave/Amplitude", see model_build.m), otherwise the name
// ("ctrl.pid.kp"). Addresses: from the ELF when it is given and knows the name / SYMBOL_LINK (preferred: an
// A2L exported before the last build has stale addresses), otherwise ECU_ADDRESS.
ElfImage a2l_image(const A2lModule& a2l, const ElfImage* elf);

std::ostream& operator<<(std::ostream& os, const A2lObject& o);
std::ostream& operator<<(std::ostream& os, const A2lModule& m);

template <>
struct std::formatter<A2lObject> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const A2lObject& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};

template <>
struct std::formatter<A2lModule> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const A2lModule& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
