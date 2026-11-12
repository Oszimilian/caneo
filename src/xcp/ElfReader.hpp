#pragma once

#include <cstdint>
#include <format>
#include <map>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Scalar types an XCP variable can be read / written as.
enum class XcpType { U8, I8, U16, I16, U32, I32, U64, I64, F32, F64, Bool, None };

const char* to_string(XcpType t);
XcpType     xcp_type_from_string(const std::string& s);   // None if unknown
uint32_t    xcp_type_size(XcpType t);
bool        xcp_type_signed(XcpType t);
bool        xcp_type_float(XcpType t);

// Raw <-> physical value of a variable (A2L COMPU_METHOD): physical = (p + q * raw) / (s + t * raw).
// Covers IDENTICAL, LINEAR (COEFFS_LINEAR a b) and RAT_FUNC without quadratic terms. Default = identity.
struct XcpConversion {
    double p = 0.0, q = 1.0, s = 1.0, t = 0.0;

    bool   identity() const { return p == 0.0 && q == 1.0 && s == 1.0 && t == 0.0; }
    double to_phys(double raw) const { return identity() ? raw : (p + q * raw) / (s + t * raw); }
    double to_raw(double phys) const;   // throws std::runtime_error if the conversion cannot be inverted there
    bool   operator==(const XcpConversion&) const = default;
};

std::ostream& operator<<(std::ostream& os, const XcpConversion& c);

// One variable of the ELF: a scalar (leaf) or a struct / union / array with children.
// Loaded from an A2L, leaves additionally carry description / unit / conversion / limits.
struct ElfVariable {
    std::string name;            // own name: "speed", "pid", "[3]"
    std::string path;            // full name: "ctrl.pid.kp", "buf[3]"
    std::string type_name;       // C type: "uint16_t", "struct pid", "float[4]"
    uint64_t    address  = 0;
    uint32_t    size     = 0;    // bytes
    XcpType     type     = XcpType::None;   // None for aggregates
    uint8_t     bit_offset = 0;  // bitfield: offset from the LSB of the storage unit at `address`
    uint8_t     bit_size   = 0;  // 0 = no bitfield
    std::vector<std::pair<int64_t, std::string>> enumerators;
    std::vector<ElfVariable> children;
    bool        truncated = false;   // array with more elements than shown

    // A2L
    std::string   description;        // LONG_IDENTIFIER (Simulink: block path, e.g. "model/Sine Wave/Amplitude")
    std::string   unit;
    XcpConversion conv;               // applied by xcp_decode / xcp_encode
    double        lower = 0.0, upper = 0.0;   // physical limits
    bool          has_limits = false;
    bool          read_only  = false;  // MEASUREMENT without READ_WRITE, CHARACTERISTIC with READ_ONLY

    bool leaf() const { return children.empty() && type != XcpType::None; }
};

struct ElfImage {
    ElfImage() = default;
    ElfImage(ElfImage&&) = default;              // by_path points into `variables`: moving keeps the elements
    ElfImage& operator=(ElfImage&&) = default;
    ElfImage(const ElfImage&) = delete;
    ElfImage& operator=(const ElfImage&) = delete;

    std::string              path;
    bool                     little_endian = true;
    bool                     has_dwarf     = false;
    bool                     from_a2l      = false;   // built by a2l_image(): leaf paths = A2L names
    unsigned                 machine       = 0;     // ELF e_machine
    std::vector<ElfVariable> variables;             // top level, sorted by name
    std::map<std::string, const ElfVariable*> by_path;   // all leaves by path (built by index())
    std::vector<std::string> warnings;

    void index();
    const ElfVariable* find(const std::string& path) const;
    std::size_t leaf_count() const { return by_path.size(); }
};

// Reads the global (and function static) variables with their types from the DWARF debug info; without debug
// info the object symbols of the symbol table are used (type guessed from the size). Throws std::runtime_error.
ElfImage load_elf(const std::string& path);

std::ostream& operator<<(std::ostream& os, const ElfVariable& v);
std::ostream& operator<<(std::ostream& os, const ElfImage& img);

template <>
struct std::formatter<XcpConversion> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const XcpConversion& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};

template <>
struct std::formatter<ElfVariable> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const ElfVariable& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};

template <>
struct std::formatter<ElfImage> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const ElfImage& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
