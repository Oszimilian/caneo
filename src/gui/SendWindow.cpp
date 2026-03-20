#include "SendWindow.hpp"

#include "action/Action.hpp"

#include <dbcppp/Network.h>
#include <algorithm>
#include <format>
#include <imgui.h>

SendWindow::SendWindow(const std::vector<InterfaceConfig>& iface_configs,
                       ActionHandler& action_handler)
    : action_handler_(action_handler)
{
    for (const auto& cfg : iface_configs) {
        if (cfg.dbc.empty())
            continue;
        ifaces_.push_back({ cfg.name, SendModel(cfg.dbc) });
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
    ImGui::Begin("Send");

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

    if (ImGui::Button("Send Once")) {
        for (int k = 0; k < (int)msg.signals.size(); ++k)
            iface.model.set_value(popup_msg_, k, editing_values_[k]);
        auto payload = iface.model.encode(popup_msg_);
        action_handler_.add_action(std::make_unique<SingleAction>(
            action_handler_.io_ref(), iface.name,
            msg.id, msg.name, std::move(payload),
            action_handler_.send_fn_ref()));
    }

    ImGui::SameLine();

    if (ImGui::Button("Send Periodic")) {
        const int period_ms = std::max(1, std::atoi(period_buf_));
        for (int k = 0; k < (int)msg.signals.size(); ++k)
            iface.model.set_value(popup_msg_, k, editing_values_[k]);
        auto payload = iface.model.encode(popup_msg_);
        action_handler_.add_action(std::make_unique<PeriodicAction>(
            action_handler_.io_ref(), iface.name,
            msg.id, msg.name, std::move(payload),
            action_handler_.send_fn_ref(),
            std::chrono::milliseconds(period_ms)));
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.f);
    ImGui::InputText("ms##period", period_buf_, sizeof(period_buf_),
                     ImGuiInputTextFlags_CharsDecimal);

    // ── Sin action ────────────────────────────────────────────────────────
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Send Sin")) {
        // Signal selector
        const char* preview = msg.signals.empty() ? "(none)"
                            : msg.signals[sin_sig_idx_].name.c_str();
        ImGui::SetNextItemWidth(200.f);
        if (ImGui::BeginCombo("Signal##sin", preview)) {
            for (int k = 0; k < (int)msg.signals.size(); ++k) {
                const bool sel = (k == sin_sig_idx_);
                if (ImGui::Selectable(msg.signals[k].name.c_str(), sel))
                    sin_sig_idx_ = k;
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const auto& ssig = msg.signals[std::min(sin_sig_idx_, (int)msg.signals.size() - 1)];
        const double lo  = ssig.min_val;
        const double hi  = ssig.max_val;
        const float  spd = (lo < hi) ? (float)((hi - lo) / 200.0) : 1.f;

        ImGui::SetNextItemWidth(200.f);
        if (lo < hi)
            ImGui::DragScalar("Amplitude##sin", ImGuiDataType_Double,
                              &sin_amplitude_, spd, &lo, &hi, "%.2f");
        else
            ImGui::InputDouble("Amplitude##sin", &sin_amplitude_, 0, 0, "%.2f");

        ImGui::SetNextItemWidth(200.f);
        if (lo < hi)
            ImGui::DragScalar("Offset##sin", ImGuiDataType_Double,
                              &sin_offset_, spd, &lo, &hi, "%.2f");
        else
            ImGui::InputDouble("Offset##sin", &sin_offset_, 0, 0, "%.2f");

        ImGui::SetNextItemWidth(220.f);
        ImGui::InputInt("Sin period (ms)##sin",  &sin_period_ms_);
        ImGui::SetNextItemWidth(220.f);
        ImGui::InputInt("Update interval (ms)##sin", &sin_interval_ms_);
        sin_period_ms_   = std::max(1, sin_period_ms_);
        sin_interval_ms_ = std::max(1, sin_interval_ms_);

        if (!msg.signals.empty() && ImGui::Button("Start Sin")) {
            // Apply current editing values to model first
            for (int k = 0; k < (int)msg.signals.size(); ++k)
                iface.model.set_value(popup_msg_, k, editing_values_[k]);

            // Build a self-contained encode_fn: captures base payload + ISignal*
            auto base_payload      = iface.model.encode(popup_msg_);
            const dbcppp::ISignal* isig = ssig.sig;
            const double           y_lo = lo;
            const double           y_hi = hi;

            SinPeriodicAction::EncodeWithValueFn encode_fn =
                [base_payload, isig, y_lo, y_hi](double v) mutable {
                    if (y_lo < y_hi)
                        v = std::clamp(v, y_lo, y_hi);
                    auto data      = base_payload;
                    const auto raw = isig->PhysToRaw(v);
                    isig->Encode(raw, data.data());
                    return data;
                };

            action_handler_.add_action(std::make_unique<SinPeriodicAction>(
                action_handler_.io_ref(), iface.name,
                msg.id, msg.name,
                action_handler_.send_fn_ref(),
                std::chrono::milliseconds(sin_interval_ms_),
                std::move(encode_fn),
                sin_amplitude_,
                std::chrono::milliseconds(sin_period_ms_),
                sin_offset_));
        }
    }

    ImGui::End();
}
