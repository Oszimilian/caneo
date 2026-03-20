#include "ActionsWindow.hpp"

#include <imgui.h>

ActionsWindow::ActionsWindow(ActionHandler& action_handler)
    : action_handler_(action_handler)
{}

void ActionsWindow::render()
{
    ImGui::Begin("Actions");
    ImGui::TextDisabled("Not yet implemented");
    ImGui::End();
}
