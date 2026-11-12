#pragma once

#include <optional>
#include <string>

// Full-screen terminal dialog "search a caneo config": browse directories from the working directory,
// Enter picks a .yaml / .yml file. nullopt if cancelled.
std::optional<std::string> pick_config_tui();
