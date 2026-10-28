#pragma once

#include <optional>
#include <string>

namespace log_viewer {

// Starts `command` in the background (via /bin/sh, so ~ and quoting work).
// "{file}" in the command is replaced by the quoted path; without the
// placeholder the path is appended. An empty `file` just starts the tool.
// Returns an error message, or nullopt on success.
std::optional<std::string> launch(const std::string& command, const std::string& file);

// Default PlotJuggler command if a PlotJuggler binary/AppImage is found in
// PATH, ~/Downloads, ~/Applications or ~/bin (newest version first).
std::optional<std::string> detect_plotjuggler();

} // namespace log_viewer
