#include "DbcModel.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>

namespace {

constexpr const char* NO_NODE = "Vector__XXX";

struct Token {
    std::string text;
    bool        quoted = false;
};

// Splits a statement into words and "quoted strings" (with \" and \\ unescaped).
std::vector<Token> tokenize(const std::string& s)
{
    std::vector<Token> out;
    std::size_t        i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (c == '"') {
            Token t;
            t.quoted = true;
            for (++i; i < s.size() && s[i] != '"'; ++i) {
                if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '"' || s[i + 1] == '\\'))
                    ++i;
                t.text += s[i];
            }
            ++i;
            out.push_back(std::move(t));
        } else {
            Token t;
            while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '"')
                t.text += s[i++];
            if (!t.text.empty() && t.text.back() == ';' && t.text.size() > 1)
                t.text.pop_back();
            if (t.text != ";")
                out.push_back(std::move(t));
        }
    }
    return out;
}

// Number of unescaped quotes.
int count_quotes(const std::string& s)
{
    int n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\')
            ++i;
        else if (s[i] == '"')
            ++n;
    }
    return n;
}

std::string escape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

double to_double(const std::string& s)
{
    try {
        return std::stod(s);
    } catch (...) {
        return 0.0;
    }
}

DbcMessage* find_message(DbcDoc& doc, uint32_t raw_id)
{
    const uint32_t id = raw_id & 0x1FFFFFFFu;
    for (auto& m : doc.messages)
        if (m.id == id)
            return &m;
    return nullptr;
}

DbcSignal* find_signal(DbcMessage& m, const std::string& name)
{
    for (auto& s : m.signals)
        if (s.name == name)
            return &s;
    return nullptr;
}

std::vector<std::string> split_receivers(const std::string& s)
{
    std::vector<std::string> out;
    std::string              cur;
    for (char c : s + ",") {
        if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty() && cur != NO_NODE)
                out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

} // namespace

std::vector<int> DbcSignal::bits() const
{
    std::vector<int> out;
    if (bit_size <= 0 || bit_size > 64)
        return out;
    int pos = start_bit;
    for (int i = 0; i < bit_size; ++i) {
        out.push_back(pos);
        if (little_endian)
            ++pos;
        else
            pos = (pos % 8 == 0) ? pos + 15 : pos - 1;
    }
    return out;
}

std::vector<int> DbcMessage::layout() const
{
    std::vector<int> cells(static_cast<std::size_t>(std::max(size, 0)) * 8, -1);
    for (std::size_t i = 0; i < signals.size(); ++i) {
        for (int b : signals[i].bits()) {
            if (b < 0 || b >= static_cast<int>(cells.size()))
                continue;
            int& cell = cells[static_cast<std::size_t>(b)];
            if (cell == -1) {
                cell = static_cast<int>(i);
            } else if (cell >= 0) {
                // multiplexed signals with different selector values may share bits
                const auto key = [](const std::string& m) { return m.size() > 1 && m.back() == 'M' ? m.substr(0, m.size() - 1) : m; };
                const auto& a = signals[static_cast<std::size_t>(cell)].mux;
                const auto& c = signals[i].mux;
                const bool  both_muxed = a.size() > 1 && c.size() > 1 && a[0] == 'm' && c[0] == 'm';
                if (!(both_muxed && key(a) != key(c)))
                    cell = -2;
            }
        }
    }
    return cells;
}

DbcDoc DbcDoc::load(const std::filesystem::path& path)
{
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Failed to open DBC file: " + path.string());
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

DbcDoc DbcDoc::parse(const std::string& text)
{
    static const std::regex bo_re(R"(^\s*BO_\s+(\d+)\s+(\w+)\s*:\s*(\d+)\s+(\S+))");
    static const std::regex sg_re(
        R"re(^\s*SG_\s+(\S+)(?:\s+(M|m\d+M?))?\s*:\s*(\d+)\|(\d+)@([01])([+-])\s*\(\s*([^,\s]+)\s*,\s*([^)\s]+)\s*\)\s*\[\s*([^|\s]*)\s*\|\s*([^\]\s]*)\s*\]\s*"([^"]*)"\s*(.*)$)re");

    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        for (std::string l; std::getline(in, l);) {
            if (!l.empty() && l.back() == '\r')
                l.pop_back();
            lines.push_back(std::move(l));
        }
    }

    DbcDoc      doc;
    DbcMessage* cur = nullptr;
    std::set<std::string> seen_nodes;
    const auto add_node = [&](const std::string& n) {
        if (!n.empty() && n != NO_NODE && seen_nodes.insert(n).second)
            doc.nodes.push_back({n, {}});
    };

    // statements that may span lines (CM_, VAL_)
    const auto statement = [&](std::size_t& i) {
        std::string s = lines[i];
        while (count_quotes(s) % 2 != 0 && i + 1 < lines.size())
            s += "\n" + lines[++i];
        if (s.rfind("VAL_", 0) == 0)
            while (trim(s).empty() || trim(s).back() != ';') {
                if (i + 1 >= lines.size())
                    break;
                s += " " + lines[++i];
            }
        return s;
    };

    struct PendingComment { std::string kind, name, text; uint32_t id; };
    std::vector<PendingComment> comments;
    struct PendingValues { uint32_t id; std::string sig; std::vector<std::pair<int64_t, std::string>> v; };
    std::vector<PendingValues> value_tables;
    struct PendingType { uint32_t id; std::string sig; int type; };
    std::vector<PendingType> value_types;
    const std::regex valtype_re(R"(^SIG_VALTYPE_\s+(\d+)\s+(\S+)\s*:\s*(\d+)\s*;)");

    bool in_ns = false;   // NS_ lists keywords (e.g. "BA_DEF_") on indented lines: not statements
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string line = trim(lines[i]);
        if (line.empty())
            continue;
        if (line.rfind("NS_", 0) == 0) {
            in_ns = true;
            continue;
        }
        if (in_ns) {
            if (lines[i][0] == ' ' || lines[i][0] == '\t')
                continue;
            in_ns = false;
        }

        if (line.rfind("VERSION", 0) == 0) {
            const auto t = tokenize(line);
            if (t.size() > 1)
                doc.version = t[1].text;
        } else if (line.rfind("BU_:", 0) == 0) {
            std::istringstream in(line.substr(4));
            for (std::string n; in >> n;)
                add_node(n);
        } else if (line.rfind("BO_ ", 0) == 0) {
            std::smatch m;
            cur = nullptr;
            if (std::regex_search(line, m, bo_re)) {
                DbcMessage msg;
                const uint64_t raw = std::stoull(m[1]);
                msg.extended = (raw & 0x80000000u) != 0;
                msg.id       = static_cast<uint32_t>(raw & 0x1FFFFFFFu);
                msg.high_bits = static_cast<uint32_t>(raw & 0x60000000u);
                msg.name     = m[2];
                msg.size     = std::stoi(m[3]);
                msg.sender   = m[4] == NO_NODE ? "" : std::string(m[4]);
                add_node(msg.sender);
                doc.messages.push_back(std::move(msg));
                cur = &doc.messages.back();
            }
        } else if (line.rfind("SG_ ", 0) == 0) {
            std::smatch m;
            if (cur && std::regex_search(line, m, sg_re)) {
                DbcSignal s;
                s.name          = m[1];
                s.mux           = m[2].matched ? std::string(m[2]) : std::string();
                s.start_bit     = std::stoi(m[3]);
                s.bit_size      = std::stoi(m[4]);
                s.little_endian = m[5] == "1";
                s.is_signed     = m[6] == "-";
                s.factor        = to_double(m[7]);
                s.offset        = to_double(m[8]);
                s.min           = to_double(m[9]);
                s.max           = to_double(m[10]);
                s.unit          = m[11];
                s.receivers     = split_receivers(m[12]);
                for (const auto& r : s.receivers)
                    add_node(r);
                cur->signals.push_back(std::move(s));
            }
        } else if (line.rfind("CM_", 0) == 0) {
            const auto t = tokenize(statement(i));
            if (t.size() >= 3 && t.back().quoted) {
                if (t[1].text == "SG_" && t.size() >= 5)
                    comments.push_back({"SG_", t[3].text, t.back().text, static_cast<uint32_t>(std::stoull(t[2].text))});
                else if (t[1].text == "BO_" && t.size() >= 4)
                    comments.push_back({"BO_", {}, t.back().text, static_cast<uint32_t>(std::stoull(t[2].text))});
                else if (t[1].text == "BU_" && t.size() >= 4)
                    comments.push_back({"BU_", t[2].text, t.back().text, 0});
            }
        } else if (line.rfind("VAL_ ", 0) == 0) {
            const auto t = tokenize(statement(i));
            if (t.size() >= 3 && !t[1].quoted) {
                PendingValues pv{static_cast<uint32_t>(std::stoull(t[1].text)), t[2].text, {}};
                for (std::size_t k = 3; k + 1 < t.size(); k += 2)
                    pv.v.emplace_back(static_cast<int64_t>(to_double(t[k].text)), t[k + 1].text);
                value_tables.push_back(std::move(pv));
            }
        } else if (line.rfind("SIG_VALTYPE_", 0) == 0) {
            std::smatch m;
            if (std::regex_search(line, m, valtype_re))
                value_types.push_back({static_cast<uint32_t>(std::stoull(m[1])), m[2], std::stoi(m[3])});
        } else if (line.rfind("BA_", 0) == 0) {
            doc.extra.push_back(statement(i));
        }
    }

    for (auto& c : comments) {
        if (c.kind == "BU_") {
            if (const int n = doc.find_node(c.name); n >= 0)
                doc.nodes[static_cast<std::size_t>(n)].comment = c.text;
        } else if (auto* m = find_message(doc, c.id)) {
            if (c.kind == "BO_")
                m->comment = c.text;
            else if (auto* s = find_signal(*m, c.name))
                s->comment = c.text;
        }
    }
    for (const auto& t : value_types)
        if (auto* m = find_message(doc, t.id))
            if (auto* s = find_signal(*m, t.sig))
                s->float_type = t.type == 1 || t.type == 2 ? t.type : 0;
    for (auto& v : value_tables)
        if (auto* m = find_message(doc, v.id))
            if (auto* s = find_signal(*m, v.sig))
                s->values = std::move(v.v);

    return doc;
}

std::string DbcDoc::to_string() const
{
    std::ostringstream o;
    o << "VERSION \"" << escape(version) << "\"\n\nNS_ :\n\nBS_:\n\nBU_:";
    for (const auto& n : nodes)
        o << ' ' << n.name;
    o << "\n\n";

    const auto raw_id = [](const DbcMessage& m) { return m.id | m.high_bits | (m.extended ? 0x80000000u : 0u); };

    for (const auto& m : messages) {
        o << "BO_ " << raw_id(m) << ' ' << m.name << ": " << m.size << ' ' << (m.sender.empty() ? NO_NODE : m.sender) << '\n';
        for (const auto& s : m.signals) {
            o << " SG_ " << s.name << (s.mux.empty() ? "" : " " + s.mux) << " : " << s.start_bit << '|' << s.bit_size << '@'
              << (s.little_endian ? '1' : '0') << (s.is_signed ? '-' : '+') << " (" << std::format("{}", s.factor) << ','
              << std::format("{}", s.offset) << ") [" << std::format("{}", s.min) << '|' << std::format("{}", s.max)
              << "] \"" << escape(s.unit) << "\" ";
            if (s.receivers.empty())
                o << NO_NODE;
            for (std::size_t i = 0; i < s.receivers.size(); ++i)
                o << (i ? "," : "") << s.receivers[i];
            o << '\n';
        }
        o << '\n';
    }

    for (const auto& n : nodes)
        if (!n.comment.empty())
            o << "CM_ BU_ " << n.name << " \"" << escape(n.comment) << "\";\n";
    for (const auto& m : messages) {
        if (!m.comment.empty())
            o << "CM_ BO_ " << raw_id(m) << " \"" << escape(m.comment) << "\";\n";
        for (const auto& s : m.signals)
            if (!s.comment.empty())
                o << "CM_ SG_ " << raw_id(m) << ' ' << s.name << " \"" << escape(s.comment) << "\";\n";
    }
    for (const auto& e : extra)
        o << e << '\n';
    for (const auto& m : messages)
        for (const auto& s : m.signals)
            if (s.float_type != 0)
                o << "SIG_VALTYPE_ " << raw_id(m) << ' ' << s.name << " : " << s.float_type << ";\n";
    for (const auto& m : messages)
        for (const auto& s : m.signals) {
            if (s.values.empty())
                continue;
            o << "VAL_ " << raw_id(m) << ' ' << s.name;
            for (const auto& [v, d] : s.values)
                o << ' ' << v << " \"" << escape(d) << '"';
            o << " ;\n";
        }
    return o.str();
}

void DbcDoc::save(const std::filesystem::path& path) const
{
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open())
        throw std::runtime_error("Failed to write DBC file: " + path.string());
    f << to_string();
}

int DbcDoc::find_node(const std::string& name) const
{
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].name == name)
            return static_cast<int>(i);
    return -1;
}

void DbcDoc::rename_node(std::size_t idx, const std::string& new_name)
{
    if (idx >= nodes.size())
        return;
    const std::string old = nodes[idx].name;
    nodes[idx].name       = new_name;
    for (auto& m : messages) {
        if (m.sender == old)
            m.sender = new_name;
        for (auto& s : m.signals)
            for (auto& r : s.receivers)
                if (r == old)
                    r = new_name;
    }
}

void DbcDoc::remove_node(std::size_t idx)
{
    if (idx >= nodes.size())
        return;
    const std::string old = nodes[idx].name;
    nodes.erase(nodes.begin() + static_cast<std::ptrdiff_t>(idx));
    for (auto& m : messages) {
        if (m.sender == old)
            m.sender.clear();
        for (auto& s : m.signals)
            std::erase(s.receivers, old);
    }
}

std::vector<std::string> DbcDoc::problems() const
{
    std::vector<std::string> out;
    std::map<uint64_t, std::string> ids;
    std::set<std::string>           names;
    std::set<std::string>           node_names;

    for (const auto& n : nodes) {
        if (n.name.empty())
            out.push_back("a node has no name");
        else if (!node_names.insert(n.name).second)
            out.push_back("duplicate node " + n.name);
    }
    for (const auto& m : messages) {
        const std::string tag = "message " + m.name;
        const uint64_t    key = m.id | (m.extended ? 1ull << 32 : 0);
        if (m.name.empty())
            out.push_back("a message has no name");
        else if (!names.insert(m.name).second)
            out.push_back("duplicate message name " + m.name);
        if (auto [it, ok] = ids.emplace(key, m.name); !ok)
            out.push_back(std::format("{}: ID 0x{:X} is also used by {}", tag, m.id, it->second));
        if (!m.extended && m.id > 0x7FF)
            out.push_back(tag + ": ID > 0x7FF needs 'extended'");
        if (m.size < 1 || m.size > 64)
            out.push_back(tag + ": size must be 1..64 bytes");
        if (!m.sender.empty() && find_node(m.sender) < 0)
            out.push_back(tag + ": unknown sender " + m.sender);

        std::set<std::string> sig_names;
        for (const auto& s : m.signals) {
            const std::string st = tag + "." + s.name;
            if (s.name.empty())
                out.push_back(tag + ": a signal has no name");
            else if (!sig_names.insert(s.name).second)
                out.push_back(st + ": duplicate signal name");
            if (s.bit_size < 1 || s.bit_size > 64) {
                out.push_back(st + ": size must be 1..64 bits");
                continue;
            }
            const int total = m.size * 8;
            for (int b : s.bits())
                if (b < 0 || b >= total) {
                    out.push_back(st + ": does not fit into the message");
                    break;
                }
            if (s.float_type == 1 && s.bit_size != 32)
                out.push_back(st + ": float needs 32 bits");
            if (s.float_type == 2 && s.bit_size != 64)
                out.push_back(st + ": double needs 64 bits");
            if (s.max < s.min)
                out.push_back(st + ": max < min");
        }
        const auto cells = m.layout();
        if (std::ranges::find(cells, -2) != cells.end())
            out.push_back(tag + ": signals overlap");
    }
    return out;
}

std::ostream& operator<<(std::ostream& os, const DbcDoc& doc)
{
    return os << "DbcDoc(" << doc.nodes.size() << " nodes, " << doc.messages.size() << " messages)";
}
