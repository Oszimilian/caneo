#include "CsvExport.hpp"

#include <format>
#include <fstream>

namespace {

std::string quoted(const std::string& s)
{
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + '"';
}

} // namespace

std::string write_series_csv(const std::filesystem::path& path, const std::vector<LogDocument::Series>& series,
                             std::size_t& rows)
{
    rows = 0;
    std::ofstream f(path);
    if (!f) return "Cannot write " + path.string();
    f << "time_s,signal,unit,value\n";
    for (const auto& s : series) {
        const std::string head = "," + quoted(s.label) + "," + quoted(s.unit) + ",";
        for (std::size_t i = 0; i < s.t.size() && i < s.v.size(); ++i, ++rows)
            f << std::format("{}", s.t[i]) << head << std::format("{}", s.v[i]) << '\n';
    }
    f.flush();
    return f ? std::string{} : "Error while writing " + path.string();
}

std::string write_table_csv(const std::filesystem::path& path, const CsvTable& table, std::size_t& rows)
{
    rows = 0;
    std::ofstream f(path);
    if (!f) return "Cannot write " + path.string();
    f << "time_s";
    for (const auto& c : table.columns) f << ',' << quoted(c);
    f << '\n';
    const std::size_t cols = table.columns.size();
    for (std::size_t r = 0; r < table.t.size(); ++r, ++rows) {
        f << std::format("{}", table.t[r]);
        for (std::size_t c = 0; c < cols; ++c) f << ',' << std::format("{}", table.data[r * cols + c]);
        f << '\n';
    }
    f.flush();
    return f ? std::string{} : "Error while writing " + path.string();
}
