#include "ActionsWindow.hpp"

#include "action/Action.hpp"

#include <algorithm>
#include <chrono>
#include <dbcppp/Network.h>
#include <format>
#include <imgui.h>

ActionsWindow::ActionsWindow(ActionHandler& action_handler, SendWindow& send_window)
    : action_handler_(action_handler), send_window_(send_window)
{}

// ── helpers ───────────────────────────────────────────────────────────────────

SendModel* ActionsWindow::find_model(const std::string& interface)
{
    for (auto& iface : send_window_.ifaces())
        if (iface.name == interface)
            return &iface.model;
    return nullptr;
}

int ActionsWindow::find_msg_idx(const SendModel& model, uint64_t msg_id)
{
    const auto& msgs = model.messages();
    for (int i = 0; i < (int)msgs.size(); ++i)
        if (msgs[i].id == msg_id)
            return i;
    return -1;
}

void ActionsWindow::open_edit(const ActionInfo& info)
{
    edit_action_idx_ = info.idx;
    edit_open_       = true;

    if (info.type_name == "Sin") {
        edit_amplitude_  = info.sin_amplitude;
        edit_offset_     = info.sin_offset;
        edit_sin_period_ = (int)info.sin_period_ms;
    } else {
        // Constant — load current values from SendModel
        edit_values_.clear();
        SendModel* model = find_model(info.interface);
        if (!model) return;
        const int msg_idx = find_msg_idx(*model, info.msg_id);
        if (msg_idx < 0) return;
        for (const auto& sig : model->messages()[msg_idx].signals)
            edit_values_.push_back(sig.value);
    }
}

// ── edit window ───────────────────────────────────────────────────────────────

void ActionsWindow::render_edit_window(const std::vector<ActionInfo>& actions)
{
    if (!edit_open_) return;

    // Find the ActionInfo for the action being edited
    const ActionInfo* info = nullptr;
    for (const auto& a : actions)
        if (a.idx == edit_action_idx_) { info = &a; break; }
    if (!info) { edit_open_ = false; return; }

    const auto title = std::format("Edit Action: {} — {} (0x{:03X})",
                                   info->type_name, info->msg_name, info->msg_id);

    ImGui::SetNextWindowSize(ImVec2(480, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title.c_str(), &edit_open_)) {
        ImGui::End();
        return;
    }

    // ── Sin ──────────────────────────────────────────────────────────────────
    if (info->type_name == "Sin") {
        SendModel* model    = find_model(info->interface);
        const int  msg_idx  = model ? find_msg_idx(*model, info->msg_id) : -1;
        const bool has_range = model && msg_idx >= 0;

        // Guess bounds from sin_amplitude for sliders when DBC range not available
        double lo = has_range ? model->messages()[msg_idx].signals[0].min_val : -1e6;
        double hi = has_range ? model->messages()[msg_idx].signals[0].max_val :  1e6;
        float  spd = (lo < hi) ? (float)((hi - lo) / 200.0) : 1.f;

        ImGui::Text("Signal: %s", info->msg_name.c_str());
        ImGui::Separator();

        ImGui::SetNextItemWidth(220.f);
        if (lo < hi)
            ImGui::DragScalar("Amplitude", ImGuiDataType_Double,
                              &edit_amplitude_, spd, &lo, &hi, "%.3f");
        else
            ImGui::InputDouble("Amplitude", &edit_amplitude_, 0, 0, "%.3f");

        ImGui::SetNextItemWidth(220.f);
        if (lo < hi)
            ImGui::DragScalar("Offset", ImGuiDataType_Double,
                              &edit_offset_, spd, &lo, &hi, "%.3f");
        else
            ImGui::InputDouble("Offset", &edit_offset_, 0, 0, "%.3f");

        ImGui::SetNextItemWidth(120.f);
        ImGui::InputInt("Sin period (ms)", &edit_sin_period_);
        edit_sin_period_ = std::max(1, edit_sin_period_);

        ImGui::Spacing();
        if (ImGui::Button("Apply")) {
            action_handler_.update_sin_param(edit_action_idx_, 0, edit_amplitude_);
            action_handler_.update_sin_param(edit_action_idx_, 2, edit_offset_);
            action_handler_.update_sin_param(edit_action_idx_, 1,
                                             (double)edit_sin_period_);
        }

    // ── Constant / Periodic ──────────────────────────────────────────────────
    } else {
        SendModel* model   = find_model(info->interface);
        const int  msg_idx = model ? find_msg_idx(*model, info->msg_id) : -1;

        if (!model || msg_idx < 0) {
            ImGui::TextDisabled("No DBC available for this interface.");
            ImGui::End();
            return;
        }

        const auto& msg = model->messages()[msg_idx];

        // Ensure edit_values_ has the right size
        if ((int)edit_values_.size() != (int)msg.signals.size()) {
            edit_values_.clear();
            for (const auto& s : msg.signals)
                edit_values_.push_back(s.value);
        }

        constexpr ImGuiTableFlags tflags =
            ImGuiTableFlags_RowBg        |
            ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_BordersV     |
            ImGuiTableFlags_SizingStretchProp;

        if (ImGui::BeginTable("##edit_sigs", 3, tflags)) {
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
                const auto id = std::format("##{}", k);
                if (sig.min_val < sig.max_val) {
                    const float spd = (float)((sig.max_val - sig.min_val) / 200.0);
                    ImGui::DragScalar(id.c_str(), ImGuiDataType_Double,
                                      &edit_values_[k], spd,
                                      &sig.min_val, &sig.max_val, "%.2f");
                } else {
                    ImGui::InputDouble(id.c_str(), &edit_values_[k], 0, 0, "%.2f");
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(sig.unit.c_str());
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        if (ImGui::Button("Update Payload")) {
            for (int k = 0; k < (int)msg.signals.size(); ++k)
                model->set_value(msg_idx, k, edit_values_[k]);
            action_handler_.update_payload(edit_action_idx_,
                                           model->encode(msg_idx));
        }
    }

    ImGui::End();
}

// ── main render ───────────────────────────────────────────────────────────────

void ActionsWindow::render()
{
    ImGui::Begin("Actions");

    const auto actions = action_handler_.snapshot();

    if (actions.empty()) {
        ImGui::TextDisabled("No active actions.");
        ImGui::End();
        render_edit_window(actions);
        return;
    }

    constexpr ImGuiTableFlags tflags =
        ImGuiTableFlags_ScrollY      |
        ImGuiTableFlags_RowBg        |
        ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_BordersV     |
        ImGuiTableFlags_Resizable    |
        ImGuiTableFlags_SizingStretchProp;

    if (!ImGui::BeginTable("##actions", 8, tflags)) {
        ImGui::End();
        render_edit_window(actions);
        return;
    }

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Type",      ImGuiTableColumnFlags_WidthFixed,   80.f);
    ImGui::TableSetupColumn("Interface", ImGuiTableColumnFlags_WidthFixed,   100.f);
    ImGui::TableSetupColumn("ID",        ImGuiTableColumnFlags_WidthFixed,   80.f);
    ImGui::TableSetupColumn("Name",      ImGuiTableColumnFlags_WidthStretch, 3.f);
    ImGui::TableSetupColumn("Period",    ImGuiTableColumnFlags_WidthFixed,   80.f);
    ImGui::TableSetupColumn("Last sent", ImGuiTableColumnFlags_WidthFixed,   100.f);
    ImGui::TableSetupColumn("##pause",   ImGuiTableColumnFlags_WidthFixed,   100.f);
    ImGui::TableSetupColumn("##delete",  ImGuiTableColumnFlags_WidthFixed,   80.f);
    ImGui::TableHeadersRow();

    const auto  now      = std::chrono::steady_clock::now();
    std::size_t to_remove = SIZE_MAX;
    std::size_t to_toggle = SIZE_MAX;

    for (const auto& a : actions) {
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        const bool selected = edit_open_ && edit_action_idx_ == a.idx;
        const auto sel_id   = std::format("{}##sel_{}", a.type_name, a.idx);
        if (ImGui::Selectable(sel_id.c_str(), selected))
            open_edit(a);

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(a.interface.c_str());

        ImGui::TableSetColumnIndex(2);
        ImGui::Text("0x%03llX", (unsigned long long)a.msg_id);

        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(a.msg_name.c_str());

        ImGui::TableSetColumnIndex(4);
        if (a.is_periodic)
            ImGui::Text("%lldms", (long long)a.period.count());
        else
            ImGui::TextDisabled("once");

        ImGui::TableSetColumnIndex(5);
        if (!a.ever_sent) {
            ImGui::TextDisabled("never");
        } else {
            const double ms =
                std::chrono::duration<double, std::milli>(now - a.last_sent).count();
            ImGui::Text("%.0fms ago", ms);
        }

        ImGui::PushID((int)a.idx);

        ImGui::TableSetColumnIndex(6);
        if (a.is_periodic) {
            const char* lbl = a.paused ? "Resume" : "Pause";
            if (ImGui::Button(lbl, ImVec2(-1, 0)))
                to_toggle = a.idx;
        }

        ImGui::TableSetColumnIndex(7);
        if (ImGui::Button("Delete", ImVec2(-1, 0)))
            to_remove = a.idx;

        ImGui::PopID();
    }

    ImGui::EndTable();

    if (to_toggle != SIZE_MAX)
        action_handler_.toggle_pause(to_toggle);
    if (to_remove != SIZE_MAX) {
        if (edit_open_ && edit_action_idx_ == to_remove)
            edit_open_ = false;
        action_handler_.remove_action(to_remove);
    }

    ImGui::End();
    render_edit_window(actions);
}
