#pragma once

#include "LogDocument.hpp"
#include "config/Config.hpp"

#include <string>
#include <vector>

// Runs a FlowSpec over a loaded log instead of live traffic (Debug view).
//
// The flow is stepped on a fixed grid (step_ms) from t = 0 to the end of the
// log; `can_in` blocks read the log's signals sample-and-hold, `lua`/`generator`
// behave as live, every `scope` becomes a series "flow/<flow>/<scope>".
// `can_out` blocks are ignored (nothing is sent).
// Samples collected by a `csv` block: one row per step, one column per input.
struct CsvTable {
    int                      node_id = 0;
    std::vector<std::string> columns;
    std::vector<double>      t;
    std::vector<double>      data;   // row-major, t.size() x columns.size()
};

struct OfflineFlowResult {
    std::vector<CsvTable>            tables;    // one per csv block
    std::vector<LogDocument::Series> outputs;   // one per scope block
    std::vector<std::string>         log;       // Lua print() output
    std::string                      error;     // empty = ok
};

OfflineFlowResult run_offline_flow(const FlowSpec& spec, const LogDocument::Data& data);
