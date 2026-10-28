#include "SendWindow.hpp"

#include "flow/Flow.hpp"

#include <dbcppp/Network.h>
#include <algorithm>
#include <format>
#include <imgui.h>

SendWindow::SendWindow(const std::vector<InterfaceConfig>& iface_configs, AppState& state)
    : state_(state)
{
    for (const auto& cfg : iface_configs) {
        if (cfg.dbc.empty())
            continue;
        try {
            ifaces_.push_back({ cfg.name, SendModel(expand_path(cfg.dbc).string()) });
        } catch (const std::exception& e) {
            error_ += std::format("{}: {}\n", cfg.name, e.what());
        }
    }
}

void SendWindow::open_popup(int iface_idx, int msg_idx)
{
    popup_iface_ = iface_idx;
    popup_msg_   = msg_idx;
    popup_open_  = true;

    editing_values_.clear();
    for (const auto& sig : ifaces_[iface_idx].model.messages()[msg_idx].signals)
        editing_values_.push_back(sig.value);
}

void SendWindow::render()
{
    if (!ImGui::Begin(title(), &visible)) {
        ImGui::End();
        render_edit_window();
        return;
    }

    if (!error_.empty())
        ImGui::TextColored(ImVec4(0.85f, 0.2f, 0.1f, 1.0f), "%s", error_.c_str());

    if (ifaces_.empty()) {
        ImGui::TextDisabled("No interfaces with DBC files configured.");
        ImGui::End();
        render_edit_window();
        return;
    }

    if (ImGui::BeginTabBar("##send_ifaces")) {
        for (int i = 0; i < (int)ifaces_.size(); ++i) {
            auto& iface = ifaces_[i];
            if (!ImGui::BeginTabItem(iface.name.c_str()))
                continue;

            constexpr ImGuiTableFlags tflags =
                ImGuiTableFlags_ScrollY      |
                ImGuiTableFlags_RowBg        |
                ImGuiTableFlags_BordersOuter |
                ImGuiTableFlags_BordersV     |
                ImGuiTableFlags_Resizable    |
                ImGuiTableFlags_SizingStretchProp;

            if (ImGui::BeginTable("##msgs", 3, tflags)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("ID",   ImGuiTableColumnFlags_WidthFixed,   80.f);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.f);
                ImGui::TableSetupColumn("DLC",  ImGuiTableColumnFlags_WidthFixed,   40.f);
                ImGui::TableHeadersRow();

                const auto& msgs = iface.model.messages();
                for (int j = 0; j < (int)msgs.size(); ++j) {
                    const auto& msg = msgs[j];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);

                    const bool selected = popup_open_ &&
                                         popup_iface_ == i &&
                                         popup_msg_   == j;
                    const auto sel_id = std::format("0x{:03X}##{}_{}", msg.id, i, j);
                    if (ImGui::Selectable(sel_id.c_str(), selected,
                                         ImGuiSelectableFlags_SpanAllColumns))
                        open_popup(i, j);

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(msg.name.c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%zu", msg.dlc);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
    render_edit_window();
}

void SendWindow::render_edit_window()
{
    if (!popup_open_)
        return;
    if (popup_iface_ < 0 || popup_iface_ >= (int)ifaces_.size())
        return;

    auto& iface      = ifaces_[popup_iface_];
    const auto& msgs = iface.model.messages();
    if (popup_msg_ < 0 || popup_msg_ >= (int)msgs.size())
        return;

    const auto& msg   = msgs[popup_msg_];
    const auto  title = std::format("Edit: {} (0x{:03X})", msg.name, msg.id);

    ImGui::SetNextWindowSize(ImVec2(520, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title.c_str(), &popup_open_)) {
        ImGui::End();
        return;
    }

    constexpr ImGuiTableFlags tflags =
        ImGuiTableFlags_RowBg        |
        ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_BordersV     |
        ImGuiTableFlags_Resizable    |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("##sigs", 3, tflags)) {
        ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableSetupColumn("Value",  ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableSetupColumn("Unit",   ImGuiTableColumnFlags_WidthFixed,   60.f);
        ImGui::TableHeadersRow();

        for (int k = 0; k < (int)msg.signals.size(); ++k) {
            const auto& sig = msg.signals[k];
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(sig.name.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.f);
            const auto input_id = std::format("##{}", k);
            if (sig.min_val < sig.max_val) {
                const float speed = (float)((sig.max_val - sig.min_val) / 200.0);
                ImGui::DragScalar(input_id.c_str(), ImGuiDataType_Double,
                                  &editing_values_[k], speed,
                                  &sig.min_val, &sig.max_val, "%.2f");
            } else {
                ImGui::InputDouble(input_id.c_str(), &editing_values_[k],
                                   0.0, 0.0, "%.2f");
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(sig.unit.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::Separator();

    if (ImGui::Button("Send once")) {
        try {
            state_.send(iface.name, msg.id, iface.model.encode_values(static_cast<std::size_t>(popup_msg_), editing_values_));
            info_  = std::format("Sent {} on {}", msg.name, iface.name);
            error_.clear();
        } catch (const std::exception& e) {
            error_ = e.what();
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sends the frame once with the values above");

    ImGui::SameLine();
    if (ImGui::Button("Periodic as flow")) {
        // CAN Out block with the values as constants; waveforms etc. are built in the flow editor
        FlowSpec f;
        f.name    = std::format("Send: {}", msg.name);
        f.step_ms = 10;
        FlowNodeSpec n;
        n.id        = 1;
        n.type      = "can_out";
        n.name      = "CAN Out";
        n.interface = iface.name;
        n.msg       = msg.name;
        n.period_ms = std::max(1, std::atoi(period_buf_));
        for (int k = 0; k < (int)msg.signals.size(); ++k) {
            n.signals.push_back(msg.signals[k].name);
            n.sig_values.push_back(editing_values_[k]);
        }
        f.nodes.push_back(std::move(n));
        state_.flows().add(f);
        info_ = std::format("Flow '{}' created (see Flows)", f.name);
        error_.clear();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Creates a flow with a CAN Out block that sends this message periodically");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.f);
    ImGui::InputText("ms##period", period_buf_, sizeof(period_buf_), ImGuiInputTextFlags_CharsDecimal);

    if (!error_.empty())
        ImGui::TextColored(ImVec4(0.85f, 0.2f, 0.1f, 1.0f), "%s", error_.c_str());
    else if (!info_.empty())
        ImGui::TextDisabled("%s", info_.c_str());

    ImGui::End();
}
