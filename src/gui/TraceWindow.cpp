#include "TraceWindow.hpp"

#include <algorithm>
#include <format>
#include <imgui.h>

// ─── helpers ──────────────────────────────────────────────────────────────────

static std::string to_lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

static bool matches(uint32_t id, const CanFrame& frame, const std::string& q) {
    if (q.empty()) return true;
    const std::string lq = to_lower(q);
    auto ci = [&](const std::string& s) {
        return to_lower(s).find(lq) != std::string::npos;
    };
    if (ci(std::format("0x{:03X}", id))) return true;
    if (ci(frame.msg_name()))             return true;
    for (const auto& sig : frame.decoded())
        if (ci(sig.name) || ci(sig.unit)) return true;
    return false;
}

// ─── TraceWindow ──────────────────────────────────────────────────────────────

TraceWindow::TraceWindow(std::string interface_name, GraphWindow& graph_window)
    : set_(std::move(interface_name)), graph_window_(&graph_window)
{}

void TraceWindow::update(const CanFrame& frame)
{
    if (frame.header().interface != set_.interface())
        return;
    std::lock_guard lock(mutex_);
    set_.update(frame);
}

void TraceWindow::render()
{
    const std::string title = std::format("Trace — {}", set_.interface());
    ImGui::Begin(title.c_str());

    // Search bar
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search (ID, name, signal, unit)...", search_buf_, sizeof(search_buf_));
    const std::string query(search_buf_);
    ImGui::Separator();

    constexpr ImGuiTableFlags table_flags =
        ImGuiTableFlags_ScrollY      |
        ImGuiTableFlags_RowBg        |
        ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_BordersV     |
        ImGuiTableFlags_Resizable    |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("trace", 5, table_flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("ID",     ImGuiTableColumnFlags_WidthFixed,   80.f);
        ImGui::TableSetupColumn("Name",   ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn("DLC",    ImGuiTableColumnFlags_WidthFixed,   40.f);
        ImGui::TableSetupColumn("\xce\x94 (ms)", ImGuiTableColumnFlags_WidthFixed, 80.f); // Δ
        ImGui::TableSetupColumn("Signals / Raw", ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableHeadersRow();

        std::lock_guard lock(mutex_);
        for (const auto& [id, frame] : set_.frames()) {
            if (!matches(id, frame, query))
                continue;

            // ── frame row ──
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("0x%03X", id);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(frame.msg_name().c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", frame.header().dlc);

            ImGui::TableSetColumnIndex(3);
            if (const auto d = set_.delta(id)) {
                const double ms = std::chrono::duration<double, std::milli>(*d).count();
                ImGui::Text("%6.1f", ms);
            } else {
                ImGui::TextDisabled("---");
            }

            ImGui::TableSetColumnIndex(4);
            if (frame.decoded().empty()) {
                // Raw hex bytes
                std::string raw;
                for (uint8_t b : frame.payload())
                    raw += std::format("{:02X} ", b);
                ImGui::TextDisabled("%s", raw.c_str());
            } else {
                // First signal on the frame row itself
                const auto& sigs = frame.decoded();
                const auto& s0   = sigs[0];
                const auto  lbl0 = std::format("{}: {:.2f} {}##{}_0", s0.name, s0.value, s0.unit, id);
                ImGui::Selectable(lbl0.c_str(), false, ImGuiSelectableFlags_AllowOverlap);
                if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                    ctx_signal_     = { id, s0.name, s0.min_val, s0.max_val };
                    open_ctx_popup_ = true;
                }

                // Remaining signals as sub-rows
                for (size_t i = 1; i < sigs.size(); ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Indent(12.f);
                    const auto& s   = sigs[i];
                    const auto  lbl = std::format("{}: {:.2f} {}##{}_{}", s.name, s.value, s.unit, id, i);
                    ImGui::Selectable(lbl.c_str(), false, ImGuiSelectableFlags_AllowOverlap);
                    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                        ctx_signal_     = { id, s.name, s.min_val, s.max_val };
                        open_ctx_popup_ = true;
                    }
                    ImGui::Unindent(12.f);
                }
            }
        }

        ImGui::EndTable();
    }

    if (open_ctx_popup_) {
        ImGui::OpenPopup("##sig_ctx");
        open_ctx_popup_ = false;
    }

    if (ImGui::BeginPopup("##sig_ctx")) {
        if (ctx_signal_) {
            ImGui::Text("Signal: %s", ctx_signal_->signal_name.c_str());
            ImGui::Separator();
            const auto names = graph_window_->graph_names();
            for (size_t i = 0; i < names.size(); ++i) {
                if (ImGui::BeginMenu(names[i].c_str())) {
                    for (int a = 0; a < 3; ++a) {
                        const char* lbl[] = { "Y1", "Y2", "Y3" };
                        if (ImGui::MenuItem(lbl[a])) {
                            graph_window_->add_signal(i, set_.interface(),
                                                      ctx_signal_->msg_id,
                                                      ctx_signal_->signal_name,
                                                      ctx_signal_->y_min,
                                                      ctx_signal_->y_max, a);
                            ctx_signal_.reset();
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
                        graph_window_->add_signal(idx, set_.interface(),
                                                  ctx_signal_->msg_id,
                                                  ctx_signal_->signal_name,
                                                  ctx_signal_->y_min,
                                                  ctx_signal_->y_max, a);
                        ctx_signal_.reset();
                    }
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}
