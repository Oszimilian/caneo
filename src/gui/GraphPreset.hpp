#pragma once

#include "config/Config.hpp"
#include "gui/GraphWindow.hpp"

// Loads graph definitions from a YAML preset file and populates graph_window.
// The preset describes graphs and signals for a single interface; msg names
// are resolved to IDs via the interface's DBC file.
void load_graph_preset(const InterfaceConfig& cfg, GraphWindow& graph_window);
