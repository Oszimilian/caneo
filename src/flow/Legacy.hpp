#pragma once

#include "config/Config.hpp"

#include <string>
#include <vector>

// Older configs describe periodic / sin / Lua actions (`interfaces: <if>: actions:`) and guards
// (`guards:`) separately. The GUI runs everything as flows: this converts those entries into
// flows appended to config.flows and removes them from the config.
//
//   periodic action  CAN Out block (constants for the preset signals, period)
//   sin action       Generator -> CAN Out
//   lua action       Lua block, step() calls tick(t)
//   guard            CAN In -> Lua block (step() calls check()) -> one Scope per output
//
// Returns a line per converted entry (for the status message).
std::vector<std::string> migrate_legacy(Config& config);
