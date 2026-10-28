#pragma once

#include "OfflineFlow.hpp"

#include <filesystem>
#include <string>
#include <vector>

// CSV export of the Debug view. Both functions return an error text (empty = ok)
// and the number of data rows written.

// Every sample of every series, long format: time_s,signal,unit,value (sorted by series).
std::string write_series_csv(const std::filesystem::path& path, const std::vector<LogDocument::Series>& series,
                             std::size_t& rows);

// Result table of a CSV block of an offline flow: time_s + one column per input.
std::string write_table_csv(const std::filesystem::path& path, const CsvTable& table, std::size_t& rows);
