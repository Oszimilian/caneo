#include "A2lReader.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {

constexpr uint32_t MAX_ELEMENTS = 256;   // per array dimension shown (like the ELF reader)

struct Tok {
    std::string text;
    bool        quoted = false;
    int         line   = 0;
};

// One /begin KIND ... /end KIND block: its own tokens (keywords and values as written) and nested blocks.
struct Node {
    std::string       kind;
    std::vector<Tok>  args;
    std::vector<Node> children;
    int               line = 0;

    const std::string& arg(std::size_t i) const {
        static const std::string empty;
        return i < args.size() ? args[i].text : empty;
    }
};

std::vector<Tok> tokenize(const std::string& s)
{
    std::vector<Tok> out;
    int              line = 1;
    std::size_t      i    = 0;
    if (s.starts_with("\xEF\xBB\xBF")) i = 3;   // UTF-8 BOM
    const std::size_t n = s.size();
    while (i < n) {
        const char c = s[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            const std::size_t e = s.find("*/", i + 2);
            const std::size_t stop = e == std::string::npos ? n : e + 2;
            line += static_cast<int>(std::count(s.begin() + static_cast<std::ptrdiff_t>(i), s.begin() + static_cast<std::ptrdiff_t>(stop), '\n'));
            i = stop;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') ++i;
            continue;
        }
        Tok t;
        t.line = line;
        if (c == '"') {
            t.quoted = true;
            ++i;
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n) {
                    const char e = s[i + 1];
                    t.text += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                    i += 2;
                } else if (s[i] == '"') {
                    if (i + 1 < n && s[i + 1] == '"') { t.text += '"'; i += 2; }   // "" = quote
                    else { ++i; break; }
                } else {
                    if (s[i] == '\n') ++line;
                    t.text += s[i++];
                }
            }
        } else {
            while (i < n && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '"' &&
                   !(s[i] == '/' && i + 1 < n && (s[i + 1] == '*' || s[i + 1] == '/') && !t.text.empty()))
                t.text += s[i++];
        }
        out.push_back(std::move(t));
    }
    return out;
}

void parse_block(const std::vector<Tok>& toks, std::size_t& i, Node& node)
{
    while (i < toks.size()) {
        const Tok& t = toks[i];
        if (!t.quoted && t.text == "/begin") {
            if (i + 1 >= toks.size()) throw std::runtime_error(std::format("line {}: /begin without a keyword", t.line));
            Node child;
            child.kind = toks[i + 1].text;
            child.line = t.line;
            i += 2;
            parse_block(toks, i, child);
            node.children.push_back(std::move(child));
        } else if (!t.quoted && t.text == "/end") {
            const std::string kind = i + 1 < toks.size() ? toks[i + 1].text : "";
            if (kind != node.kind)
                throw std::runtime_error(std::format("line {}: /end {} does not close /begin {} (line {})", t.line, kind,
                                                     node.kind, node.line));
            i += 2;
            return;
        } else {
            node.args.push_back(t);
            ++i;
        }
    }
    if (!node.kind.empty())
        throw std::runtime_error(std::format("/begin {} (line {}) is never closed", node.kind, node.line));
}

std::optional<double> number(const std::string& s)
{
    if (s.empty()) return std::nullopt;
    const bool neg = s.front() == '-';
    const std::string_view body = std::string_view(s).substr(neg || s.front() == '+' ? 1 : 0);
    if (body.starts_with("0x") || body.starts_with("0X")) {
        char*          end = nullptr;
        const std::string h(body.substr(2));
        const unsigned long long u = std::strtoull(h.c_str(), &end, 16);
        if (h.empty() || *end) return std::nullopt;
        return neg ? -static_cast<double>(u) : static_cast<double>(u);
    }
    char*        end = nullptr;
    const double d   = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end) return std::nullopt;
    return d;
}

uint64_t unsigned_number(const std::string& s)
{
    const std::string_view b = s;
    if (b.starts_with("0x") || b.starts_with("0X")) return std::strtoull(s.c_str() + 2, nullptr, 16);
    return static_cast<uint64_t>(number(s).value_or(0.0));
}

XcpType a2l_type(const std::string& t)
{
    if (t == "UBYTE") return XcpType::U8;
    if (t == "SBYTE") return XcpType::I8;
    if (t == "UWORD") return XcpType::U16;
    if (t == "SWORD") return XcpType::I16;
    if (t == "ULONG") return XcpType::U32;
    if (t == "SLONG") return XcpType::I32;
    if (t == "A_UINT64") return XcpType::U64;
    if (t == "A_INT64") return XcpType::I64;
    if (t == "FLOAT32_IEEE") return XcpType::F32;
    if (t == "FLOAT64_IEEE") return XcpType::F64;
    return XcpType::None;
}

struct CompuMethod {
    XcpConversion conv;
    std::string   unit;
    std::string   tab_ref;
    bool          supported = true;
    std::string   type;
};

struct RecordLayout {
    std::string data_type;
    bool        axis_inside = false;   // axis points / counts stored in the characteristic itself
};

// Optional keywords after the fixed arguments: `fn(keyword, values)` with the values that keyword takes.
void options(const Node& n, std::size_t from, const std::function<void(const std::string&, std::vector<std::string>)>& fn)
{
    static const std::map<std::string, int> arity = {
        {"ECU_ADDRESS", 1}, {"ECU_ADDRESS_EXTENSION", 1}, {"ARRAY_SIZE", 1}, {"BIT_MASK", 1}, {"PHYS_UNIT", 1},
        {"READ_WRITE", 0}, {"READ_ONLY", 0}, {"SYMBOL_LINK", 2}, {"DISPLAY_IDENTIFIER", 1}, {"FORMAT", 1},
        {"NUMBER", 1}, {"EXTENDED_LIMITS", 2}, {"BYTE_ORDER", 1}, {"COEFFS", 6}, {"COEFFS_LINEAR", 2},
        {"COMPU_TAB_REF", 1}, {"REF_UNIT", 1}, {"DEFAULT_VALUE", 1}, {"DEFAULT_VALUE_NUMERIC", 1},
        {"MATRIX_DIM", -1}, {"FNC_VALUES", 4}, {"AXIS_PTS_X", 4}, {"AXIS_PTS_Y", 4}, {"AXIS_PTS_Z", 4},
        {"NO_AXIS_PTS_X", 2}, {"NO_AXIS_PTS_Y", 2}, {"NO_AXIS_PTS_Z", 2}, {"ALIGNMENT_BYTE", 1},
    };
    for (std::size_t i = from; i < n.args.size();) {
        const Tok& t  = n.args[i++];
        const auto it = t.quoted ? arity.end() : arity.find(t.text);
        if (it == arity.end()) continue;   // unknown keyword / value: skipped
        std::vector<std::string> vals;
        if (it->second < 0) {
            while (i < n.args.size() && !n.args[i].quoted && number(n.args[i].text)) vals.push_back(n.args[i++].text);
        } else {
            for (int k = 0; k < it->second && i < n.args.size(); ++k) vals.push_back(n.args[i++].text);
        }
        fn(t.text, std::move(vals));
    }
}

std::vector<uint32_t> dims_of(const std::vector<std::string>& vals)
{
    std::vector<uint32_t> d;
    for (const auto& v : vals) d.push_back(static_cast<uint32_t>(unsigned_number(v)));
    while (!d.empty() && d.back() <= 1) d.pop_back();   // "MATRIX_DIM 4 1 1" (A2L < 1.70 always has 3)
    std::erase(d, 0u);
    return d;
}

void walk(const Node& n, const std::function<void(const Node&)>& fn)
{
    fn(n);
    for (const auto& c : n.children) walk(c, fn);
}

// The ELF variable (leaf or aggregate) with exactly this path.
const ElfVariable* find_any(const std::vector<ElfVariable>& vars, const std::string& path)
{
    for (const auto& v : vars) {
        if (v.path == path) return &v;
        if (path.size() > v.path.size() && path.starts_with(v.path) &&
            (path[v.path.size()] == '.' || path[v.path.size()] == '['))
            return find_any(v.children, path);
    }
    return nullptr;
}

std::vector<std::string> split_path(const A2lObject& o)
{
    std::vector<std::string> parts;
    std::string              cur;
    const bool               by_long = o.long_id.find('/') != std::string::npos;
    const std::string&       s       = by_long ? o.long_id : o.name;
    const char               sep     = by_long ? '/' : '.';
    for (char c : s) {
        if (c == sep) {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur += c == '\n' ? ' ' : c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) parts.push_back(o.name);
    return parts;
}

}   // namespace

std::ostream& operator<<(std::ostream& os, const A2lObject& o)
{
    os << (o.characteristic ? "CHARACTERISTIC " : "MEASUREMENT ") << o.name << " (" << o.kind << ", " << o.data_type;
    for (auto d : o.dims) os << '[' << d << ']';
    os << " @0x" << std::hex << o.address << std::dec;
    if (!o.unit.empty()) os << ", [" << o.unit << "]";
    if (!o.conv.identity()) os << ", " << o.conv;
    if (!o.enumerators.empty()) os << ", " << o.enumerators.size() << " enumerators";
    if (o.bit_mask) os << ", mask 0x" << std::hex << o.bit_mask << std::dec;
    if (!o.long_id.empty()) os << ", \"" << o.long_id << "\"";
    return os << ")";
}

std::ostream& operator<<(std::ostream& os, const A2lModule& m)
{
    std::size_t chars = 0;
    for (const auto& o : m.objects) chars += o.characteristic;
    return os << "A2lModule(" << m.path << ", module " << m.name << ", " << m.objects.size() - chars << " measurements, "
              << chars << " characteristics)";
}

A2lModule parse_a2l(const std::string& text, std::string path)
{
    const auto toks = tokenize(text);
    Node       root;
    std::size_t i = 0;
    parse_block(toks, i, root);

    A2lModule m;
    m.path = std::move(path);

    std::map<std::string, CompuMethod>                                   methods;
    std::map<std::string, std::vector<std::pair<int64_t, std::string>>> tabs;
    std::map<std::string, RecordLayout>                                  layouts;
    std::map<std::string, std::string>                                   units;

    walk(root, [&](const Node& n) {
        if (n.kind == "MODULE") {
            if (m.name.empty()) m.name = n.arg(0);
        } else if (n.kind == "MOD_COMMON") {
            options(n, 1, [&](const std::string& k, const std::vector<std::string>& v) {
                if (k == "BYTE_ORDER" && !v.empty())
                    m.little_endian = v[0] == "MSB_LAST" || v[0] == "LITTLE_ENDIAN";
            });
        } else if (n.kind == "UNIT") {
            units[n.arg(0)] = n.arg(2);
        } else if (n.kind == "COMPU_METHOD") {
            CompuMethod cm;
            cm.type = n.arg(2);
            cm.unit = n.arg(4);
            std::vector<double> coeffs, linear;
            std::string         ref_unit;
            options(n, 5, [&](const std::string& k, const std::vector<std::string>& v) {
                if (k == "COEFFS") for (const auto& x : v) coeffs.push_back(number(x).value_or(0.0));
                else if (k == "COEFFS_LINEAR") for (const auto& x : v) linear.push_back(number(x).value_or(0.0));
                else if (k == "COMPU_TAB_REF" && !v.empty()) cm.tab_ref = v[0];
                else if (k == "REF_UNIT" && !v.empty()) ref_unit = v[0];
            });
            if (!ref_unit.empty()) cm.unit = "\x01" + ref_unit;   // resolved below (UNIT may come later)
            if (cm.type == "LINEAR" && linear.size() == 2) {
                cm.conv = {linear[1], linear[0], 1.0, 0.0};   // phys = a * raw + b
            } else if (cm.type == "RAT_FUNC" && coeffs.size() == 6) {
                // raw = (a p^2 + b p + c) / (d p^2 + e p + f); without p^2: phys = (c - f raw) / (e raw - b)
                const double a = coeffs[0], b = coeffs[1], c = coeffs[2], d = coeffs[3], e = coeffs[4], f = coeffs[5];
                if (a == 0.0 && d == 0.0 && (b * f - c * e) != 0.0) cm.conv = {c, -f, -b, e};
                else cm.supported = false;
            } else if (cm.type != "IDENTICAL" && cm.type != "TAB_VERB") {
                cm.supported = cm.type.empty();
            }
            methods[n.arg(0)] = std::move(cm);
        } else if (n.kind == "COMPU_VTAB") {
            auto&             t     = tabs[n.arg(0)];
            const std::size_t count = static_cast<std::size_t>(number(n.arg(3)).value_or(0.0));
            for (std::size_t k = 0; k < count && 5 + 2 * k < n.args.size(); ++k)
                t.emplace_back(static_cast<int64_t>(number(n.arg(4 + 2 * k)).value_or(0.0)), n.arg(5 + 2 * k));
        } else if (n.kind == "COMPU_VTAB_RANGE") {
            auto&             t     = tabs[n.arg(0)];
            const std::size_t count = static_cast<std::size_t>(number(n.arg(2)).value_or(0.0));
            for (std::size_t k = 0; k < count; ++k) {
                const double lo = number(n.arg(3 + 3 * k)).value_or(0.0), hi = number(n.arg(4 + 3 * k)).value_or(0.0);
                if (lo == hi) t.emplace_back(static_cast<int64_t>(lo), n.arg(5 + 3 * k));
            }
        } else if (n.kind == "RECORD_LAYOUT") {
            RecordLayout rl;
            options(n, 1, [&](const std::string& k, const std::vector<std::string>& v) {
                if (k == "FNC_VALUES" && v.size() >= 2) rl.data_type = v[1];
                else if (k.starts_with("AXIS_PTS_") || k.starts_with("NO_AXIS_PTS_")) rl.axis_inside = true;
            });
            layouts[n.arg(0)] = std::move(rl);
        }
    });
    for (auto& [name, cm] : methods)
        if (cm.unit.starts_with('\x01')) {
            const auto it = units.find(cm.unit.substr(1));
            cm.unit = it == units.end() ? cm.unit.substr(1) : it->second;
        }

    std::size_t unsupported_conv = 0, skipped_axis = 0, unknown_type = 0;
    const auto apply_method = [&](A2lObject& o, const std::string& name) {
        const auto it = methods.find(name);
        if (it == methods.end()) return;   // NO_COMPU_METHOD / unknown = identity
        const auto& cm = it->second;
        if (!cm.supported) ++unsupported_conv;
        else o.conv = cm.conv;
        o.unit = cm.unit;
        if (!cm.tab_ref.empty())
            if (const auto t = tabs.find(cm.tab_ref); t != tabs.end()) o.enumerators = t->second;
    };

    walk(root, [&](const Node& n) {
        if (n.kind != "MEASUREMENT" && n.kind != "CHARACTERISTIC") return;
        A2lObject o;
        o.name           = n.arg(0);
        o.long_id        = n.arg(1);
        o.characteristic = n.kind == "CHARACTERISTIC";
        std::size_t from = 0;
        std::string method;
        if (o.characteristic) {
            o.kind    = n.arg(2);
            o.address = unsigned_number(n.arg(3));
            method    = n.arg(6);
            o.lower   = number(n.arg(7)).value_or(0.0);
            o.upper   = number(n.arg(8)).value_or(0.0);
            const auto rl = layouts.find(n.arg(4));
            if (rl != layouts.end()) {
                o.data_type = rl->second.data_type;
                if (rl->second.axis_inside && o.kind != "VALUE" && o.kind != "VAL_BLK" && o.kind != "ASCII") {
                    ++skipped_axis;
                    return;
                }
            }
            from = 9;
        } else {
            o.kind      = "MEASUREMENT";
            o.data_type = n.arg(2);
            method      = n.arg(3);
            o.lower     = number(n.arg(6)).value_or(0.0);
            o.upper     = number(n.arg(7)).value_or(0.0);
            o.read_only = true;
            from        = 8;
        }
        options(n, from, [&](const std::string& k, const std::vector<std::string>& v) {
            if (k == "ECU_ADDRESS" && !v.empty()) o.address = unsigned_number(v[0]);
            else if (k == "BIT_MASK" && !v.empty()) o.bit_mask = unsigned_number(v[0]);
            else if (k == "ARRAY_SIZE" || k == "MATRIX_DIM" || k == "NUMBER") { if (o.dims.empty()) o.dims = dims_of(v); }
            else if (k == "READ_WRITE") o.read_only = false;
            else if (k == "READ_ONLY") o.read_only = true;
            else if (k == "SYMBOL_LINK" && v.size() == 2) {
                o.symbol        = v[0];
                o.symbol_offset = static_cast<int64_t>(number(v[1]).value_or(0.0));
            }
        });
        if (o.dims.empty() && (o.kind == "CURVE" || o.kind == "MAP" || o.kind == "CUBOID"))
            for (const auto& c : n.children)
                if (c.kind == "AXIS_DESCR") {
                    const auto pts = static_cast<uint32_t>(number(c.arg(3)).value_or(0.0));
                    if (pts > 1) o.dims.push_back(pts);
                }
        o.type = a2l_type(o.data_type);
        if (o.type == XcpType::None) {
            ++unknown_type;
            return;
        }
        apply_method(o, method);
        options(n, from, [&](const std::string& k, const std::vector<std::string>& v) {
            if (k == "PHYS_UNIT" && !v.empty()) o.unit = v[0];
        });
        m.objects.push_back(std::move(o));
    });

    if (unsupported_conv)
        m.warnings.push_back(std::format("{} object(s) with a conversion caneo cannot apply (TAB_INTP / FORM / "
                                         "quadratic RAT_FUNC) - shown as raw values", unsupported_conv));
    if (skipped_axis)
        m.warnings.push_back(std::format("{} curve(s) / map(s) with axis points in the record layout skipped", skipped_axis));
    if (unknown_type) m.warnings.push_back(std::format("{} object(s) with an unsupported data type skipped", unknown_type));
    if (m.objects.empty()) m.warnings.push_back("no MEASUREMENT / CHARACTERISTIC found");
    return m;
}

A2lModule load_a2l(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    try {
        return parse_a2l(ss.str(), path);
    } catch (const std::exception& e) {
        throw std::runtime_error(path + ": " + e.what());
    }
}

ElfImage a2l_image(const A2lModule& a2l, const ElfImage* elf)
{
    ElfImage img;
    img.path          = a2l.path;
    img.from_a2l      = true;
    img.has_dwarf     = elf && elf->has_dwarf;
    img.little_endian = elf ? elf->little_endian : a2l.little_endian.value_or(true);
    img.machine       = elf ? elf->machine : 0;
    img.warnings      = a2l.warnings;

    // tree of groups (path components) with the objects as leaves
    struct Group {
        std::map<std::string, Group> groups;
        std::vector<ElfVariable>     leaves;
    };
    Group       root;
    std::size_t from_elf = 0, stale = 0, unresolved = 0, size_mismatch = 0;

    for (const auto& o : a2l.objects) {
        ElfVariable v;
        const auto  parts = split_path(o);
        v.name        = parts.back();
        v.path        = o.name;
        v.description = o.long_id;
        v.unit        = o.unit;
        v.conv        = o.conv;
        v.enumerators = o.enumerators;
        v.lower       = o.lower;
        v.upper       = o.upper;
        v.has_limits  = o.lower < o.upper;
        v.read_only   = o.read_only;
        v.type        = o.type;
        v.size        = xcp_type_size(o.type);
        v.type_name   = to_string(o.type);
        if (o.bit_mask) {
            v.bit_offset = static_cast<uint8_t>(std::countr_zero(o.bit_mask));
            v.bit_size   = static_cast<uint8_t>(std::popcount(o.bit_mask));
        }

        // address: the ELF knows the current build, the A2L may be older
        uint64_t address = o.address;
        const ElfVariable* ev = nullptr;
        if (elf) {
            ev = find_any(elf->variables, o.symbol.empty() ? o.name : o.symbol);
            if (!ev && !o.symbol.empty()) ev = find_any(elf->variables, o.name);
        }
        if (ev) {
            const uint64_t a = ev->address + static_cast<uint64_t>(ev->path == o.symbol ? o.symbol_offset : 0);
            if (o.address && o.address != a) ++stale;
            address = a;
            ++from_elf;
            if (o.dims.empty() && !o.bit_mask && ev->leaf() && ev->size != v.size) ++size_mismatch;
        } else if (!o.address) {
            ++unresolved;
        }
        v.address = address;

        if (!o.dims.empty()) {
            // array: one leaf per element (row-major as in C), at most MAX_ELEMENTS per dimension
            ElfVariable arr = v;
            arr.type        = XcpType::None;
            arr.children.clear();
            uint64_t total  = 1;
            for (auto d : o.dims) total *= d;
            arr.size = static_cast<uint32_t>(total * v.size);
            arr.type_name = to_string(o.type);
            for (auto d : o.dims) arr.type_name += std::format("[{}]", d);
            std::vector<uint32_t> idx(o.dims.size(), 0);
            const std::function<void(std::size_t, uint64_t, std::string, std::vector<ElfVariable>&)> add =
                [&](std::size_t dim, uint64_t flat, std::string suffix, std::vector<ElfVariable>& out) {
                    const uint32_t n = std::min(o.dims[dim], MAX_ELEMENTS);
                    if (o.dims[dim] > MAX_ELEMENTS) arr.truncated = true;
                    for (uint32_t k = 0; k < n; ++k) {
                        const uint64_t    f = flat * o.dims[dim] + k;
                        const std::string s = suffix + std::format("[{}]", k);
                        if (dim + 1 == o.dims.size()) {
                            ElfVariable e = v;
                            e.name        = std::format("[{}]", k);
                            e.path        = o.name + s;
                            e.description = o.long_id.empty() ? "" : o.long_id + s;
                            e.address     = address + f * v.size;
                            out.push_back(std::move(e));
                        } else {
                            ElfVariable row;
                            row.name      = std::format("[{}]", k);
                            row.path      = o.name + s;
                            row.address   = address + f * v.size * [&] {
                                uint64_t r = 1;
                                for (std::size_t q = dim + 1; q < o.dims.size(); ++q) r *= o.dims[q];
                                return r;
                            }();
                            row.type_name = arr.type_name;
                            add(dim + 1, f, s, row.children);
                            out.push_back(std::move(row));
                        }
                    }
                };
            add(0, 0, "", arr.children);
            v = std::move(arr);
        }

        Group* g = &root;
        for (std::size_t k = 0; k + 1 < parts.size(); ++k) g = &g->groups[parts[k]];
        g->leaves.push_back(std::move(v));
    }

    // groups first (sorted), then the objects in A2L order
    const std::function<std::vector<ElfVariable>(Group&, const std::string&)> build = [&](Group& g, const std::string& prefix) {
        std::vector<ElfVariable> out;
        for (auto& [name, sub] : g.groups) {
            ElfVariable node;
            node.name      = name;
            node.path      = prefix.empty() ? name : prefix + "/" + name;
            node.type_name = "";
            node.children  = build(sub, node.path);
            out.push_back(std::move(node));
        }
        for (auto& l : g.leaves) out.push_back(std::move(l));
        return out;
    };
    img.variables = build(root, "");

    if (elf && stale)
        img.warnings.push_back(std::format("{} ECU_ADDRESS(es) differ from the ELF - the A2L is older than the build; "
                                           "using the ELF addresses", stale));
    if (unresolved)
        img.warnings.push_back(std::format("{} object(s) without an address{}", unresolved,
                                           elf ? " (not in the ELF either)" : " - load the ELF as well"));
    if (size_mismatch) img.warnings.push_back(std::format("{} object(s) have another size in the ELF", size_mismatch));
    if (elf) img.warnings.push_back(std::format("{} of {} addresses from the ELF", from_elf, a2l.objects.size()));
    img.index();
    return img;
}
