#pragma once

#include "config/Config.hpp"
#include "gui/GraphWindow.hpp"

// Loads graph definitions from the top-level graphs_preset file and populates
// graph_window.  Each signal entry may specify an 'interface:' key; if omitted,
// all interfaces are searched in order for a matching message name.
void load_graph_preset(const Config& config, GraphWindow& graph_window);
