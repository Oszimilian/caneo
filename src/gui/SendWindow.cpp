#include "SendWindow.hpp"

#include <imgui.h>

SendWindow::SendWindow(const std::vector<InterfaceConfig>& iface_configs,
                       ActionHandler& action_handler)
    : action_handler_(action_handler)
{}

void SendWindow::render()
{
    ImGui::Begin("Send");
    ImGui::TextDisabled("Not yet implemented");
    ImGui::End();
}
