#include "TraceContainerWindow.hpp"

#include <imgui.h>

void TraceContainerWindow::add(std::unique_ptr<TraceWindow> tw)
{
    traces_.push_back(std::move(tw));
}

void TraceContainerWindow::set_add_guard_callback(TraceWindow::AddGuardFn cb)
{
    for (auto& t : traces_)
        t->set_add_guard_callback(cb);
}

void TraceContainerWindow::update(const CanFrame& frame)
{
    for (auto& t : traces_)
        t->update(frame);
}

void TraceContainerWindow::render()
{
    if (!ImGui::Begin(title(), &visible)) {
        ImGui::End();
        return;
    }

    if (traces_.empty()) {
        ImGui::TextDisabled("No interfaces configured - add one in Settings.");
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
