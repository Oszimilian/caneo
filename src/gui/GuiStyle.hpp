#pragma once

#include <string>

struct ImFont;

namespace gui_style {

// Rebuilds fonts and style for the given scale and theme ("light"/"dark").
// Must be called outside ImGui::NewFrame()/Render(). Pass backend_ready=true
// once the OpenGL backend is initialised so the font texture is re-uploaded.
void apply(float scale, const std::string& theme, bool backend_ready);

// Monospace font (script editor); nullptr before the first apply().
ImFont* mono_font();

} // namespace gui_style
