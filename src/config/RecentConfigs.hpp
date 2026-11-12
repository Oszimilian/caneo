#pragma once

#include <string>
#include <vector>

// Recently used config files, kept in ~/.config/caneo/recent_configs (independent of any caneo.yaml),
// newest first, max 20. recent_configs() only returns files that still exist.
std::vector<std::string> recent_configs();
void                     remember_config(const std::string& path);
void                     forget_config(const std::string& path);
