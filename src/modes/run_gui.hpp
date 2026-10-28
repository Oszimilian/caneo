#pragma once

#include "config/AppConfig.hpp"

// Runs the Dear ImGui front end. Returns true if the user asked for a
// restart (e.g. after changing CAN channels); app.config_path then names the
// config file to restart with.
bool run_gui(AppConfig& app);
