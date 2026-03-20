#include "ModelWindow.hpp"

#include <imgui.h>

ModelWindow::ModelWindow(GraphWindow& graph_window)
    : graph_window_(&graph_window)
{}

void ModelWindow::push(const std::string& name, double value)
{
    {
        std::lock_guard lock(mutex_);
        bool found = false;
        for (auto& kv : values_) {
            if (kv.first == name) { kv.second = value; found = true; break; }
        }
        if (!found)
            values_.emplace_back(name, value);
    }
    graph_window_->push_value(IFACE, name, value);
}

void ModelWindow::render()
{
    ImGui::Begin("Model");

    {
        std::lock_guard lock(mutex_);
        if (values_.empty()) {
            ImGui::TextDisabled("No model output yet.");
            ImGui::End();
            return;
        }

        constexpr ImGuiTableFlags tflags =
            ImGuiTableFlags_ScrollY      |
            ImGuiTableFlags_RowBg        |
            ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_BordersV     |
            ImGuiTableFlags_Resizable    |
            ImGuiTableFlags_SizingStretchProp;

        if (ImGui::BeginTable("##model_out", 2, tflags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch, 3.f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 2.f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < (int)values_.size(); ++i) {
                const auto& [name, val] = values_[i];
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                const auto sel_id = name + "##" + std::to_string(i);
                ImGui::Selectable(sel_id.c_str(), false, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_SpanAllColumns);
                if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                    ctx_name_       = name;
                    open_ctx_popup_ = true;
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.6g", val);
            }
            ImGui::EndTable();
        }
    }

    if (open_ctx_popup_) {
        ImGui::OpenPopup("##model_ctx");
        open_ctx_popup_ = false;
    }

    if (ImGui::BeginPopup("##model_ctx")) {
        if (ctx_name_) {
            ImGui::Text("Signal: %s", ctx_name_->c_str());
            ImGui::Separator();
            const auto names = graph_window_->graph_names();
            for (size_t i = 0; i < names.size(); ++i) {
                if (ImGui::BeginMenu(names[i].c_str())) {
                    for (int a = 0; a < 3; ++a) {
                        const char* lbl[] = { "Y1", "Y2", "Y3" };
                        if (ImGui::MenuItem(lbl[a])) {
                            graph_window_->add_signal(i, IFACE, 0, *ctx_name_, 0.0, 0.0, a);
                            ctx_name_.reset();
                        }
                    }
                    ImGui::EndMenu();
                }
            }
            if (!names.empty())
                ImGui::Separator();
            if (ImGui::BeginMenu("+ New Graph")) {
                for (int a = 0; a < 3; ++a) {
                    const char* lbl[] = { "Y1", "Y2", "Y3" };
                    if (ImGui::MenuItem(lbl[a])) {
                        const size_t idx = graph_window_->add_graph();
                        graph_window_->add_signal(idx, IFACE, 0, *ctx_name_, 0.0, 0.0, a);
                        ctx_name_.reset();
                    }
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}
