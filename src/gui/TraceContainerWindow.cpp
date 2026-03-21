#include "TraceContainerWindow.hpp"

#include <imgui.h>

void TraceContainerWindow::add(std::unique_ptr<TraceWindow> tw)
{
    traces_.push_back(std::move(tw));
}

void TraceContainerWindow::update(const CanFrame& frame)
{
    for (auto& t : traces_)
        t->update(frame);
}

void TraceContainerWindow::render()
{
    ImGui::Begin("Trace");

    if (traces_.size() == 1) {
        traces_[0]->render_content();
    } else if (ImGui::BeginTabBar("##trace_ifaces")) {
        for (auto& t : traces_) {
            if (ImGui::BeginTabItem(t->interface_name().c_str())) {
                t->render_content();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}
