#include "ElfReader.hpp"

#include <dwarf.h>
#include <libdwarf.h>
#include <elfio/elfio.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>

// ── types ────────────────────────────────────────────────────────────────────

const char* to_string(XcpType t)
{
    switch (t) {
        case XcpType::U8:   return "uint8";
        case XcpType::I8:   return "int8";
        case XcpType::U16:  return "uint16";
        case XcpType::I16:  return "int16";
        case XcpType::U32:  return "uint32";
        case XcpType::I32:  return "int32";
        case XcpType::U64:  return "uint64";
        case XcpType::I64:  return "int64";
        case XcpType::F32:  return "float";
        case XcpType::F64:  return "double";
        case XcpType::Bool: return "bool";
        case XcpType::None: return "-";
    }
    return "-";
}

XcpType xcp_type_from_string(const std::string& s)
{
    for (XcpType t : {XcpType::U8, XcpType::I8, XcpType::U16, XcpType::I16, XcpType::U32, XcpType::I32,
                      XcpType::U64, XcpType::I64, XcpType::F32, XcpType::F64, XcpType::Bool})
        if (s == to_string(t)) return t;
    return XcpType::None;
}

uint32_t xcp_type_size(XcpType t)
{
    switch (t) {
        case XcpType::U8: case XcpType::I8: case XcpType::Bool: return 1;
        case XcpType::U16: case XcpType::I16: return 2;
        case XcpType::U32: case XcpType::I32: case XcpType::F32: return 4;
        case XcpType::U64: case XcpType::I64: case XcpType::F64: return 8;
        case XcpType::None: return 0;
    }
    return 0;
}

bool xcp_type_signed(XcpType t)
{
    return t == XcpType::I8 || t == XcpType::I16 || t == XcpType::I32 || t == XcpType::I64;
}

bool xcp_type_float(XcpType t) { return t == XcpType::F32 || t == XcpType::F64; }

static XcpType int_type(uint64_t size, bool is_signed)
{
    switch (size) {
        case 1: return is_signed ? XcpType::I8 : XcpType::U8;
        case 2: return is_signed ? XcpType::I16 : XcpType::U16;
        case 4: return is_signed ? XcpType::I32 : XcpType::U32;
        case 8: return is_signed ? XcpType::I64 : XcpType::U64;
        default: return XcpType::None;
    }
}

// ── conversion ───────────────────────────────────────────────────────────────

double XcpConversion::to_raw(double phys) const
{
    if (identity()) return phys;
    const double den = t * phys - q;
    if (den == 0.0) throw std::runtime_error(std::format("{} cannot be converted to a raw value", phys));
    return (p - s * phys) / den;
}

std::ostream& operator<<(std::ostream& os, const XcpConversion& c)
{
    if (c.identity()) return os << "identity";
    if (c.t == 0.0) return os << "phys = " << c.q / c.s << " * raw + " << c.p / c.s;
    return os << "phys = (" << c.p << " + " << c.q << " * raw) / (" << c.s << " + " << c.t << " * raw)";
}

// ── image ────────────────────────────────────────────────────────────────────

void ElfImage::index()
{
    by_path.clear();
    std::function<void(const ElfVariable&)> add = [&](const ElfVariable& v) {
        if (v.leaf()) by_path.emplace(v.path, &v);
        for (const auto& c : v.children) add(c);
    };
    for (const auto& v : variables) add(v);
}

const ElfVariable* ElfImage::find(const std::string& path) const
{
    const auto it = by_path.find(path);
    return it == by_path.end() ? nullptr : it->second;
}

std::ostream& operator<<(std::ostream& os, const ElfVariable& v)
{
    os << v.path << " @0x" << std::hex << v.address << std::dec << " (" << v.type_name << ", " << v.size << " B";
    if (v.bit_size) os << ", bits " << int(v.bit_offset) << ".." << int(v.bit_offset + v.bit_size - 1);
    if (!v.children.empty()) os << ", " << v.children.size() << " children";
    if (!v.unit.empty()) os << ", [" << v.unit << "]";
    if (!v.conv.identity()) os << ", " << v.conv;
    if (!v.description.empty()) os << ", \"" << v.description << "\"";
    return os << ")";
}

std::ostream& operator<<(std::ostream& os, const ElfImage& img)
{
    return os << "ElfImage(" << img.path << ", " << img.variables.size() << " variables, " << img.leaf_count()
              << " scalars, " << (img.has_dwarf ? "DWARF" : "symbols only") << ", "
              << (img.little_endian ? "little" : "big") << " endian)";
}

// ── DWARF ────────────────────────────────────────────────────────────────────

namespace {

constexpr std::size_t MAX_ARRAY_ELEMENTS = 256;    // per array dimension shown
constexpr std::size_t MAX_LEAVES         = 200'000;
constexpr int         MAX_DEPTH          = 16;

// Owns one DIE.
struct Die {
    Dwarf_Die d = nullptr;
    Die() = default;
    explicit Die(Dwarf_Die x) : d(x) {}
    Die(Die&& o) noexcept : d(std::exchange(o.d, nullptr)) {}
    Die& operator=(Die&& o) noexcept { if (this != &o) { reset(); d = std::exchange(o.d, nullptr); } return *this; }
    Die(const Die&) = delete;
    Die& operator=(const Die&) = delete;
    ~Die() { reset(); }
    void reset() { if (d) dwarf_dealloc_die(d); d = nullptr; }
    explicit operator bool() const { return d != nullptr; }
};

struct Attr {
    Dwarf_Attribute a = nullptr;
    ~Attr() { if (a) dwarf_dealloc_attribute(a); }
};

struct Member {
    std::string name;
    uint64_t    offset   = 0;    // bytes from the start of the aggregate
    Dwarf_Off   type     = 0;
    bool        has_type = false;
    uint8_t     bit_size = 0, bit_lsb = 0;   // bitfield: storage unit at offset, size storage_size
    uint8_t     storage_size = 0;
};

struct TypeDesc {
    enum Kind { Scalar, Struct, Union, Array, Void, Unknown } kind = Unknown;
    std::string name;
    uint64_t    size   = 0;
    XcpType     scalar = XcpType::None;
    std::vector<std::pair<int64_t, std::string>> enumerators;
    std::vector<Member>   members;
    std::vector<uint64_t> dims;          // array
    Dwarf_Off             elem = 0;      // array element type
    bool                  has_elem = false;
};

class DwarfReader {
public:
    DwarfReader(Dwarf_Debug dbg, bool little_endian) : dbg_(dbg), le_(little_endian) {}

    void read(std::vector<ElfVariable>& out, std::map<std::string, uint64_t>& missing_address);

private:
    std::string name_of(Dwarf_Die die)
    {
        char* n = nullptr;
        return dwarf_diename(die, &n, &err_) == DW_DLV_OK && n ? n : "";
    }
    Dwarf_Half tag_of(Dwarf_Die die)
    {
        Dwarf_Half t = 0;
        dwarf_tag(die, &t, &err_);
        return t;
    }
    bool has(Dwarf_Die die, Dwarf_Half at)
    {
        Dwarf_Bool b = 0;
        return dwarf_hasattr(die, at, &b, &err_) == DW_DLV_OK && b;
    }
    std::optional<uint64_t> udata(Dwarf_Die die, Dwarf_Half at)
    {
        Attr a;
        if (dwarf_attr(die, at, &a.a, &err_) != DW_DLV_OK) return std::nullopt;
        Dwarf_Unsigned u = 0;
        if (dwarf_formudata(a.a, &u, &err_) == DW_DLV_OK) return u;
        Dwarf_Signed s = 0;
        if (dwarf_formsdata(a.a, &s, &err_) == DW_DLV_OK) return static_cast<uint64_t>(s);
        return std::nullopt;
    }
    std::optional<int64_t> sdata(Dwarf_Die die, Dwarf_Half at)
    {
        Attr a;
        if (dwarf_attr(die, at, &a.a, &err_) != DW_DLV_OK) return std::nullopt;
        Dwarf_Half form = 0;
        dwarf_whatform(a.a, &form, &err_);
        Dwarf_Signed s = 0;
        Dwarf_Unsigned u = 0;
        if (form == DW_FORM_sdata && dwarf_formsdata(a.a, &s, &err_) == DW_DLV_OK) return s;
        if (dwarf_formudata(a.a, &u, &err_) == DW_DLV_OK) return static_cast<int64_t>(u);
        if (dwarf_formsdata(a.a, &s, &err_) == DW_DLV_OK) return s;
        return std::nullopt;
    }
    std::optional<Dwarf_Off> ref(Dwarf_Die die, Dwarf_Half at)
    {
        Attr a;
        if (dwarf_attr(die, at, &a.a, &err_) != DW_DLV_OK) return std::nullopt;
        Dwarf_Off off = 0;
        if (dwarf_global_formref(a.a, &off, &err_) != DW_DLV_OK) return std::nullopt;
        return off;
    }
    Die at_offset(Dwarf_Off off)
    {
        Dwarf_Die d = nullptr;
        if (dwarf_offdie_b(dbg_, off, true, &d, &err_) != DW_DLV_OK) return {};
        return Die(d);
    }
    Die child(Dwarf_Die die)
    {
        Dwarf_Die c = nullptr;
        return dwarf_child(die, &c, &err_) == DW_DLV_OK ? Die(c) : Die{};
    }
    Die sibling(Dwarf_Die die)
    {
        Dwarf_Die s = nullptr;
        return dwarf_siblingof_c(die, &s, &err_) == DW_DLV_OK ? Die(s) : Die{};
    }
    template <typename Fn>
    void for_children(Dwarf_Die die, Fn&& fn)
    {
        for (Die c = child(die); c; c = sibling(c.d)) fn(c.d);
    }

    // Single-op location expressions: DW_OP_addr <a> (variables) / DW_OP_plus_uconst <n> (DWARF 2 members).
    std::optional<uint64_t> location_op(Dwarf_Die die, Dwarf_Half at, Dwarf_Small want)
    {
        Attr a;
        if (dwarf_attr(die, at, &a.a, &err_) != DW_DLV_OK) return std::nullopt;
        Dwarf_Loc_Head_c head = nullptr;
        Dwarf_Unsigned   count = 0;
        if (dwarf_get_loclist_c(a.a, &head, &count, &err_) != DW_DLV_OK) return std::nullopt;
        std::optional<uint64_t> result;
        if (count == 1) {
            Dwarf_Small lle = 0, source = 0;
            Dwarf_Unsigned raw_lo = 0, raw_hi = 0, ops = 0, bytes = 0, expr_off = 0, desc_off = 0;
            Dwarf_Bool unavailable = 0;
            Dwarf_Addr lo = 0, hi = 0;
            Dwarf_Locdesc_c desc = nullptr;
            if (dwarf_get_locdesc_entry_e(head, 0, &lle, &raw_lo, &raw_hi, &unavailable, &lo, &hi, &ops, &bytes,
                                          &desc, &source, &expr_off, &desc_off, &err_) == DW_DLV_OK && ops == 1) {
                Dwarf_Small op = 0;
                Dwarf_Unsigned o1 = 0, o2 = 0, o3 = 0, br = 0;
                if (dwarf_get_location_op_value_c(desc, 0, &op, &o1, &o2, &o3, &br, &err_) == DW_DLV_OK && op == want)
                    result = o1;
            }
        }
        dwarf_dealloc_loc_head_c(head);
        return result;
    }

    const TypeDesc* resolve(Dwarf_Off off, int depth = 0);
    void            fill_struct(Dwarf_Die die, TypeDesc& t);
    void            fill_array(Dwarf_Die die, TypeDesc& t);

    void walk(Dwarf_Die parent, const std::string& prefix, Dwarf_Half addr_size, std::vector<ElfVariable>& out,
              std::map<std::string, uint64_t>& missing);
    void variable(Dwarf_Die die, const std::string& prefix, std::vector<ElfVariable>& out,
                  std::map<std::string, uint64_t>& missing);
    void build(ElfVariable& v, const TypeDesc* t, int depth);

    Dwarf_Debug dbg_;
    Dwarf_Error err_ = nullptr;
    bool        le_;
    Dwarf_Half  addr_size_ = 4;
    std::unordered_map<Dwarf_Off, std::unique_ptr<TypeDesc>> types_;
    std::set<std::string> seen_;
    std::map<std::string, Dwarf_Off> declared_;   // declaration only (address from the symbol table)
    std::size_t leaves_ = 0;
};

void DwarfReader::fill_struct(Dwarf_Die die, TypeDesc& t)
{
    for_children(die, [&](Dwarf_Die m) {
        if (tag_of(m) != DW_TAG_member) return;
        if (has(m, DW_AT_external) && !has(m, DW_AT_data_member_location)) return;   // C++ static member
        Member mem;
        mem.name = name_of(m);
        if (auto ty = ref(m, DW_AT_type)) { mem.type = *ty; mem.has_type = true; }
        if (auto loc = udata(m, DW_AT_data_member_location)) mem.offset = *loc;
        else if (auto op = location_op(m, DW_AT_data_member_location, DW_OP_plus_uconst)) mem.offset = *op;

        if (auto bits = udata(m, DW_AT_bit_size); bits && *bits > 0 && *bits <= 64) {
            mem.bit_size = static_cast<uint8_t>(*bits);
            if (auto dbo = udata(m, DW_AT_data_bit_offset)) {        // DWARF 4+: from the start of the struct
                const uint64_t first = *dbo % 8;
                const uint64_t need  = (first + *bits + 7) / 8;
                const uint8_t  n     = need <= 1 ? 1 : need <= 2 ? 2 : need <= 4 ? 4 : 8;
                mem.offset       = *dbo / 8;
                mem.storage_size = n;
                mem.bit_lsb      = static_cast<uint8_t>(le_ ? first : n * 8 - first - *bits);
            } else {                                                  // DWARF 2/3: from the MSB of the storage unit
                const uint64_t bo = udata(m, DW_AT_bit_offset).value_or(0);
                uint64_t bs = udata(m, DW_AT_byte_size).value_or(0);
                if (bs == 0 && mem.has_type)
                    if (const auto* mt = resolve(mem.type)) bs = mt->size;
                if (bs == 0 || bs > 8) bs = 4;
                mem.storage_size = static_cast<uint8_t>(bs);
                mem.bit_lsb      = static_cast<uint8_t>(bs * 8 - bo - *bits);
            }
        }
        t.members.push_back(std::move(mem));
    });
}

void DwarfReader::fill_array(Dwarf_Die die, TypeDesc& t)
{
    for_children(die, [&](Dwarf_Die s) {
        const auto tg = tag_of(s);
        if (tg != DW_TAG_subrange_type && tg != DW_TAG_enumeration_type) return;
        uint64_t n = 0;
        if (auto c = udata(s, DW_AT_count)) n = *c;
        else if (auto ub = sdata(s, DW_AT_upper_bound)) {
            const int64_t lb = sdata(s, DW_AT_lower_bound).value_or(0);
            n = *ub >= lb ? static_cast<uint64_t>(*ub - lb + 1) : 0;
        }
        t.dims.push_back(n);
    });
    if (t.dims.empty()) t.dims.push_back(0);
}

const TypeDesc* DwarfReader::resolve(Dwarf_Off off, int depth)
{
    if (auto it = types_.find(off); it != types_.end()) return it->second.get();
    if (depth > 32) return nullptr;
    Die die = at_offset(off);
    if (!die) return nullptr;

    auto  owned = std::make_unique<TypeDesc>();
    auto& t     = *owned;
    types_[off] = std::move(owned);   // placeholder against cycles (struct with a pointer to itself)

    const auto        tag   = tag_of(die.d);
    const std::string name  = name_of(die.d);
    const auto        under = ref(die.d, DW_AT_type);
    t.size = udata(die.d, DW_AT_byte_size).value_or(0);

    switch (tag) {
        case DW_TAG_base_type: {
            t.kind = TypeDesc::Scalar;
            t.name = name;
            const auto enc = udata(die.d, DW_AT_encoding).value_or(0);
            if (enc == DW_ATE_float)
                t.scalar = t.size == 4 ? XcpType::F32 : t.size == 8 ? XcpType::F64 : XcpType::None;
            else if (enc == DW_ATE_boolean)
                t.scalar = t.size == 1 ? XcpType::Bool : int_type(t.size, false);
            else if (enc == DW_ATE_signed || enc == DW_ATE_signed_char)
                t.scalar = int_type(t.size, true);
            else
                t.scalar = int_type(t.size, false);
            if (t.scalar == XcpType::None) t.kind = TypeDesc::Unknown;   // long double, complex, ...
            break;
        }
        case DW_TAG_enumeration_type: {
            t.kind = TypeDesc::Scalar;
            t.name = name.empty() ? "enum" : "enum " + name;
            bool is_signed = false;
            if (under)
                if (const auto* u = resolve(*under, depth + 1)) {
                    is_signed = xcp_type_signed(u->scalar);
                    if (t.size == 0) t.size = u->size;
                }
            for_children(die.d, [&](Dwarf_Die e) {
                if (tag_of(e) != DW_TAG_enumerator) return;
                const int64_t v = sdata(e, DW_AT_const_value).value_or(0);
                if (v < 0) is_signed = true;
                t.enumerators.emplace_back(v, name_of(e));
            });
            if (t.size == 0) t.size = 4;
            t.scalar = int_type(t.size, is_signed);
            break;
        }
        case DW_TAG_pointer_type:
        case DW_TAG_reference_type:
        case DW_TAG_rvalue_reference_type:
        case DW_TAG_ptr_to_member_type: {
            t.kind = TypeDesc::Scalar;
            if (t.size == 0) t.size = addr_size_;
            t.scalar = int_type(t.size, false);
            std::string target = "void";
            if (under)
                if (Die u = at_offset(*under)) {
                    const std::string un = name_of(u.d);
                    if (!un.empty()) target = un;
                }
            t.name = target + "*";
            break;
        }
        case DW_TAG_typedef:
        case DW_TAG_const_type:
        case DW_TAG_volatile_type:
        case DW_TAG_restrict_type:
        case DW_TAG_atomic_type:
        case DW_TAG_immutable_type:
        case DW_TAG_packed_type: {
            const TypeDesc* u = under ? resolve(*under, depth + 1) : nullptr;
            if (!u) { t.kind = TypeDesc::Void; t.name = tag == DW_TAG_typedef ? name : "void"; break; }
            const std::string own = tag == DW_TAG_typedef ? name : u->name;
            t = *u;   // copy (members / dims refer to offsets, not to this desc)
            t.name = own;
            break;
        }
        case DW_TAG_structure_type:
        case DW_TAG_class_type:
        case DW_TAG_union_type: {
            t.kind = tag == DW_TAG_union_type ? TypeDesc::Union : TypeDesc::Struct;
            t.name = (tag == DW_TAG_union_type ? "union " : "struct ") + (name.empty() ? std::string("{...}") : name);
            if (has(die.d, DW_AT_declaration)) { t.kind = TypeDesc::Unknown; break; }   // incomplete type
            fill_struct(die.d, t);
            break;
        }
        case DW_TAG_array_type: {
            t.kind = TypeDesc::Array;
            if (under) { t.elem = *under; t.has_elem = true; }
            fill_array(die.d, t);
            const TypeDesc* e = t.has_elem ? resolve(t.elem, depth + 1) : nullptr;
            std::string dims;
            uint64_t    count = 1;
            for (auto d : t.dims) { dims += "[" + std::to_string(d) + "]"; count *= d; }
            t.name = (e ? e->name : std::string("?")) + dims;
            if (t.size == 0 && e) t.size = e->size * count;
            break;
        }
        default:
            t.kind = TypeDesc::Unknown;
            t.name = name.empty() ? "?" : name;
            break;
    }
    return &t;
}

// Fills v (address, size and name already set) from the type: scalar or children.
void DwarfReader::build(ElfVariable& v, const TypeDesc* t, int depth)
{
    if (!t) { v.type_name = "?"; return; }
    v.type_name = t->name;
    if (v.size == 0) v.size = static_cast<uint32_t>(t->size);
    if (depth > MAX_DEPTH || leaves_ > MAX_LEAVES) { v.truncated = true; return; }

    switch (t->kind) {
        case TypeDesc::Scalar:
            v.type        = t->scalar;
            v.enumerators = t->enumerators;
            ++leaves_;
            break;
        case TypeDesc::Struct:
        case TypeDesc::Union:
            for (const auto& m : t->members) {
                const TypeDesc* mt = m.has_type ? resolve(m.type) : nullptr;
                if (m.name.empty() && mt && (mt->kind == TypeDesc::Struct || mt->kind == TypeDesc::Union)) {
                    // anonymous struct / union: its members belong to this level
                    ElfVariable anon;
                    anon.path    = v.path;
                    anon.address = v.address + m.offset;
                    build(anon, mt, depth + 1);
                    for (auto& c : anon.children) v.children.push_back(std::move(c));   // paths start with v.path
                    continue;
                }
                ElfVariable c;
                c.name    = m.name.empty() ? "<anon>" : m.name;
                c.path    = v.path + "." + c.name;
                c.address = v.address + m.offset;
                if (m.bit_size) {
                    c.size       = m.storage_size;
                    c.bit_size   = m.bit_size;
                    c.bit_offset = m.bit_lsb;
                    c.type_name  = (mt ? mt->name : std::string("int")) + ":" + std::to_string(m.bit_size);
                    const bool sgn = mt && xcp_type_signed(mt->scalar);
                    c.type = m.bit_size == 1 && mt && mt->scalar == XcpType::Bool ? XcpType::Bool
                                                                                 : int_type(m.storage_size, sgn);
                    if (mt) c.enumerators = mt->enumerators;
                    ++leaves_;
                } else {
                    build(c, mt, depth + 1);
                }
                v.children.push_back(std::move(c));
            }
            break;
        case TypeDesc::Array: {
            const TypeDesc* e = t->has_elem ? resolve(t->elem) : nullptr;
            if (!e || e->size == 0) break;
            // outer dimension = children; inner dimensions as a nested array type
            const uint64_t n      = t->dims.empty() ? 0 : t->dims.front();
            uint64_t       stride = e->size;
            for (std::size_t i = 1; i < t->dims.size(); ++i) stride *= t->dims[i];
            TypeDesc inner;
            if (t->dims.size() > 1) {
                inner      = *t;
                inner.dims = std::vector<uint64_t>(t->dims.begin() + 1, t->dims.end());
                inner.size = stride;
                std::string dims;
                for (auto d : inner.dims) dims += "[" + std::to_string(d) + "]";
                inner.name = e->name + dims;
            }
            const TypeDesc* et = t->dims.size() > 1 ? &inner : e;
            const uint64_t shown = std::min<uint64_t>(n, MAX_ARRAY_ELEMENTS);
            v.truncated = shown < n;
            for (uint64_t i = 0; i < shown; ++i) {
                ElfVariable c;
                c.name    = "[" + std::to_string(i) + "]";
                c.path    = v.path + c.name;
                c.address = v.address + i * stride;
                c.size    = static_cast<uint32_t>(stride);
                build(c, et, depth + 1);
                v.children.push_back(std::move(c));
            }
            break;
        }
        case TypeDesc::Void:
        case TypeDesc::Unknown:
            break;
    }
}

void DwarfReader::variable(Dwarf_Die die, const std::string& prefix, std::vector<ElfVariable>& out,
                           std::map<std::string, uint64_t>& missing)
{
    // name / type may live in the declaration (DW_AT_specification) or the abstract origin (inlined functions)
    std::string            name = name_of(die);
    std::optional<Dwarf_Off> type = ref(die, DW_AT_type);
    for (Dwarf_Half at : {DW_AT_specification, DW_AT_abstract_origin}) {
        if (!name.empty() && type) break;
        if (auto o = ref(die, at))
            if (Die d = at_offset(*o)) {
                if (name.empty()) name = name_of(d.d);
                if (!type) type = ref(d.d, DW_AT_type);
            }
    }
    if (name.empty() || !type) return;
    const std::string path = prefix + name;

    const auto addr = location_op(die, DW_AT_location, DW_OP_addr);
    if (!addr) {
        // extern declaration (or an address we cannot read): try the symbol table later
        if (!has(die, DW_AT_const_value) && prefix.empty() && !declared_.contains(path)) declared_[path] = *type;
        return;
    }
    if (!seen_.insert(path).second) return;
    declared_.erase(path);

    ElfVariable v;
    v.name    = path;
    v.path    = path;
    v.address = *addr;
    build(v, resolve(*type), 0);
    out.push_back(std::move(v));
    (void)missing;
}

void DwarfReader::walk(Dwarf_Die parent, const std::string& prefix, Dwarf_Half addr_size,
                       std::vector<ElfVariable>& out, std::map<std::string, uint64_t>& missing)
{
    for_children(parent, [&](Dwarf_Die d) {
        switch (tag_of(d)) {
            case DW_TAG_variable:
                variable(d, prefix, out, missing);
                break;
            case DW_TAG_namespace: {
                const std::string n = name_of(d);
                walk(d, prefix + (n.empty() ? "" : n + "::"), addr_size, out, missing);
                break;
            }
            case DW_TAG_subprogram:
            case DW_TAG_lexical_block: {   // function static variables: "func.var"
                std::string n = name_of(d);
                if (n.empty())
                    if (auto o = ref(d, DW_AT_specification).or_else([&] { return ref(d, DW_AT_abstract_origin); }))
                        if (Die s = at_offset(*o)) n = name_of(s.d);
                const std::string p = tag_of(d) == DW_TAG_subprogram ? (n.empty() ? prefix : prefix + n + ".") : prefix;
                walk(d, p, addr_size, out, missing);
                break;
            }
            default:
                break;
        }
    });
}

void DwarfReader::read(std::vector<ElfVariable>& out, std::map<std::string, uint64_t>& missing)
{
    for (;;) {
        Dwarf_Die      cu = nullptr;
        Dwarf_Unsigned hdr_len = 0, typeoff = 0, next = 0;
        Dwarf_Half     version = 0, addr_size = 0, len_size = 0, ext_size = 0, cu_type = 0;
        Dwarf_Off      abbrev = 0;
        Dwarf_Sig8     sig{};
        const int res = dwarf_next_cu_header_e(dbg_, true, &cu, &hdr_len, &version, &abbrev, &addr_size, &len_size,
                                               &ext_size, &sig, &typeoff, &next, &cu_type, &err_);
        if (res != DW_DLV_OK) break;
        Die cu_die(cu);
        addr_size_ = addr_size ? addr_size : 4;
        walk(cu_die.d, "", addr_size_, out, missing);
    }
    // declarations without a definition in the debug info: address from the symbol table (filled by the caller)
    for (const auto& [path, type] : declared_) {
        auto it = missing.find(path);
        if (it == missing.end()) continue;
        ElfVariable v;
        v.name = v.path = path;
        v.address = it->second;
        build(v, resolve(type), 0);
        out.push_back(std::move(v));
        seen_.insert(path);
    }
}

void dwarf_error_handler(Dwarf_Error, Dwarf_Ptr) {}

}   // namespace

// ── loading ──────────────────────────────────────────────────────────────────

ElfImage load_elf(const std::string& path)
{
    ElfImage img;
    img.path = path;

    ELFIO::elfio elf;
    if (!elf.load(path)) throw std::runtime_error("Not an ELF file: " + path);
    img.little_endian = elf.get_encoding() == ELFIO::ELFDATA2LSB;
    img.machine       = elf.get_machine();

    // object symbols: name -> (address, size)
    struct Sym { uint64_t address; uint64_t size; };
    std::map<std::string, Sym> symbols;
    for (const auto& sec : elf.sections) {
        if (sec->get_type() != ELFIO::SHT_SYMTAB) continue;
        const ELFIO::symbol_section_accessor syms(elf, sec.get());
        for (ELFIO::Elf_Xword i = 0; i < syms.get_symbols_num(); ++i) {
            std::string       name;
            ELFIO::Elf64_Addr value = 0;
            ELFIO::Elf_Xword  size  = 0;
            unsigned char     bind = 0, type = 0, other = 0;
            ELFIO::Elf_Half   shndx = 0;
            if (!syms.get_symbol(i, name, value, size, bind, type, shndx, other)) continue;
            if (type != ELFIO::STT_OBJECT || name.empty() || shndx == ELFIO::SHN_UNDEF) continue;
            symbols.emplace(name, Sym{value, size});
        }
    }
    std::map<std::string, uint64_t> sym_address;
    for (const auto& [n, s] : symbols) sym_address[n] = s.address;

    Dwarf_Debug dbg = nullptr;
    Dwarf_Error err = nullptr;
    const int res = dwarf_init_path(path.c_str(), nullptr, 0, DW_GROUPNUMBER_ANY, dwarf_error_handler, nullptr, &dbg, &err);
    if (res == DW_DLV_OK) {
        img.has_dwarf = true;
        try {
            DwarfReader reader(dbg, img.little_endian);
            reader.read(img.variables, sym_address);
        } catch (...) {
            dwarf_finish(dbg);
            throw;
        }
        dwarf_finish(dbg);
    } else if (res == DW_DLV_ERROR) {
        img.warnings.push_back(std::string("DWARF: ") + dwarf_errmsg(err));
        dwarf_dealloc_error(dbg, err);
    }
    if (img.variables.empty()) img.has_dwarf = false;

    if (!img.has_dwarf) {
        img.warnings.push_back("No debug info (build with -g): variable types are guessed from the symbol size");
        for (const auto& [name, s] : symbols) {
            if (s.size == 0) continue;
            ElfVariable v;
            v.name = v.path = name;
            v.address = s.address;
            v.size    = static_cast<uint32_t>(s.size);
            if (s.size == 1 || s.size == 2 || s.size == 4 || s.size == 8) {
                v.type      = int_type(s.size, false);
                v.type_name = std::string("? (") + to_string(v.type) + ")";
            } else {
                v.type_name = std::format("? ({} bytes)", s.size);
                const uint64_t shown = std::min<uint64_t>(s.size, MAX_ARRAY_ELEMENTS);
                v.truncated = shown < s.size;
                for (uint64_t i = 0; i < shown; ++i) {
                    ElfVariable c;
                    c.name      = "[" + std::to_string(i) + "]";
                    c.path      = name + c.name;
                    c.address   = s.address + i;
                    c.size      = 1;
                    c.type      = XcpType::U8;
                    c.type_name = "uint8";
                    v.children.push_back(std::move(c));
                }
            }
            img.variables.push_back(std::move(v));
        }
    }

    std::ranges::sort(img.variables, {}, &ElfVariable::path);
    img.index();
    return img;
}
