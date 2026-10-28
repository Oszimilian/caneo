#include "FlowsWindow.hpp"

#include "FlowEditorWindow.hpp"

#include <imgui.h>

namespace {

const ImVec4 COL_ERROR  = ImVec4(0.85f, 0.20f, 0.10f, 1.0f);
const ImVec4 COL_ACTIVE = ImVec4(0.15f, 0.60f, 0.20f, 1.0f);

const char* const GUARD_CODE = R"(-- Guard: watches the input signal.
-- step(u, t) returns a table; every key is an output (see the "outputs" field).
local limit = 100

function step(u, t)
    return {
        too_high = u > limit,   -- booleans become 1 / 0
        margin   = limit - u,
    }
end
)";

} // namespace

FlowSpec FlowsWindow::guard_template(const std::string& iface, const std::string& msg, const std::string& signal)
{
    FlowSpec f;
    f.name    = std::format("Guard: {}.{}", msg, signal);
    f.step_ms = 10;

    FlowNodeSpec in;
    in.id = 1; in.type = "can_in"; in.name = "CAN In"; in.x = 0; in.y = 0;
    in.interface = iface; in.msg = msg; in.signals = {signal};

    FlowNodeSpec lua;
    lua.id = 2; lua.type = "lua"; lua.name = "check"; lua.x = 320; lua.y = 0;
    lua.interface = iface;
    lua.signals   = {"too_high", "margin"};
    lua.code      = GUARD_CODE;

    f.nodes = {in, lua};
    f.links.push_back({1, 0, 2, 0});
    for (int i = 0; i < 2; ++i) {
        FlowNodeSpec sc;
        sc.id = 3 + i; sc.type = "scope"; sc.x = 700; sc.y = static_cast<float>(i) * 90;
        sc.name = std::format("guard/{}/{}", signal, lua.signals[static_cast<std::size_t>(i)]);
        f.nodes.push_back(sc);
        f.links.push_back({2, i, sc.id, 0});
    }
    return f;
}

void FlowsWindow::render()
{
    if (!ImGui::Begin(title(), &visible)) {
        ImGui::End();
        return;
    }

    const auto flows = state_.flows().snapshot();

    if (ImGui::Button("+ New")) {
        editor_.start_new_flow();
        editor_.visible = true;
        ImGui::SetWindowFocus(editor_.title());
    }
    ImGui::SameLine();
    if (ImGui::Button("Run all"))
        for (const auto& f : flows) if (!f.spec.enabled) state_.flows().set_enabled(f.idx, true);
    ImGui::SameLine();
    if (ImGui::Button("Stop all"))
        for (const auto& f : flows) if (f.spec.enabled) state_.flows().set_enabled(f.idx, false);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu flows", flows.size());
    ImGui::Separator();

    if (flows.empty())
        ImGui::TextDisabled("No flows yet. New creates one; right-click a signal in the trace -> Add guard.");

    std::optional<std::size_t> remove;
    std::optional<std::size_t> duplicate;
    if (ImGui::BeginChild("##flowlist")) {
        for (const auto& f : flows) {
            ImGui::PushID(static_cast<int>(f.idx));

            bool on = f.spec.enabled;
            if (ImGui::Checkbox("##run", &on))
                state_.flows().set_enabled(f.idx, on);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Run / stop");
            ImGui::SameLine();

            const std::string err = f.state->log.error();
            const bool        bad = !err.empty() || !f.loaded;
            if (bad) ImGui::PushStyleColor(ImGuiCol_Text, COL_ERROR);
            const bool selected = editor_.current_flow() == static_cast<int>(f.idx);
            if (ImGui::Selectable(f.spec.name.c_str(), selected)) {
                editor_.open_flow(f.idx);
                editor_.visible = true;
                ImGui::SetWindowFocus(editor_.title());
            }
            if (bad) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && bad)
                ImGui::SetTooltip("%s", err.empty() ? "not loaded" : err.c_str());

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("running").x * 0.5f);
            if (bad)               ImGui::TextColored(COL_ERROR, "error");
            else if (f.running)    ImGui::TextColored(COL_ACTIVE, "running");
            else                   ImGui::TextDisabled("stopped");

            if (ImGui::BeginPopupContextItem("##ctx")) {
                if (ImGui::MenuItem("Open")) {
                    editor_.open_flow(f.idx);
                    editor_.visible = true;
                    ImGui::SetWindowFocus(editor_.title());
                }
                if (ImGui::MenuItem("Duplicate")) duplicate = f.idx;
                if (ImGui::MenuItem("Delete")) remove = f.idx;
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (duplicate && *duplicate < flows.size()) {
        FlowSpec copy = flows[*duplicate].spec;
        copy.name += " copy";
        state_.flows().add(copy);
    }
    if (remove) {
        state_.flows().remove(*remove);
        editor_.flow_removed(*remove);
    }
    ImGui::End();
}
