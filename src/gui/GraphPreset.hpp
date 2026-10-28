#pragma once

#include "config/Config.hpp"
#include "gui/GraphWindow.hpp"
#include "send/SendModel.hpp"

#include <functional>
#include <memory>
#include <string>

// Returns the DBC model of an interface (nullptr if none).
using ModelLookup = std::function<std::shared_ptr<const SendModel>(const std::string& iface)>;

// Adds the graphs of config.graphs to graph_window. A signal without an
// 'interface:' is searched in all interfaces in order. Signals whose message
// cannot be found are kept (marked unresolved) so saving does not lose them.
void apply_graph_config(const Config& config, GraphWindow& graph_window, const ModelLookup& models);
