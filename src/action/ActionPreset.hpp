#pragma once

#include "ActionHandler.hpp"
#include "config/Config.hpp"

// Loads all actions defined in cfg.actions_file and adds them to handler
// in a paused state. Does nothing if cfg.actions_file or cfg.dbc is empty.
void load_action_presets(const InterfaceConfig& cfg, ActionHandler& handler);
