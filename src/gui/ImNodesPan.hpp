#pragma once

#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <imnodes.h>

// Navigation + zoom for an imnodes editor (imnodes itself only pans, with the middle button).
//
// Zoom is emulated: the font of the editor window, the ImGui/imnodes style sizes and the node positions are
// scaled by `zoom`. Node positions are stored unscaled by the caller; use to_grid()/from_grid().
//
// Per frame, for one editor:
//   const ImVec2 origin = ImGui::GetCursorScreenPos();
//   zoom.begin();                       // before BeginNodeEditor()
//   ImNodes::BeginNodeEditor(); zoom.inside(); ...nodes...; ImNodes::EndNodeEditor();
//   zoom.end();
//   const bool add_click = zoom.navigate(hovered, origin);   // right click on empty canvas
struct ImNodesZoom {
    float zoom = 1.0f;
    bool  changed = false;   // zoom changed this frame: re-push the node positions

    // Once, after the context was created (theme colours do not matter, only the sizes).
    void capture()
    {
        base_ = ImNodes::GetStyle();
        have_base_ = true;
    }

    ImVec2 to_grid(float x, float y) const { return ImVec2(x * zoom, y * zoom); }
    ImVec2 from_grid(ImVec2 p) const { return ImVec2(p.x / zoom, p.y / zoom); }

    void begin()
    {
        if (!have_base_) capture();
        const ImGuiStyle& s = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(s.ItemSpacing.x * zoom, s.ItemSpacing.y * zoom));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.FramePadding.x * zoom, s.FramePadding.y * zoom));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(s.ItemInnerSpacing.x * zoom, s.ItemInnerSpacing.y * zoom));

        ImNodesStyle& n = ImNodes::GetStyle();
        n.GridSpacing           = base_.GridSpacing * zoom;
        n.NodeCornerRounding    = base_.NodeCornerRounding * zoom;
        n.NodePadding           = ImVec2(base_.NodePadding.x * zoom, base_.NodePadding.y * zoom);
        n.NodeBorderThickness   = std::max(1.0f, base_.NodeBorderThickness * zoom);
        n.LinkThickness         = std::max(1.0f, base_.LinkThickness * zoom);
        n.LinkLineSegmentsPerLength = base_.LinkLineSegmentsPerLength;
        n.LinkHoverDistance     = base_.LinkHoverDistance * zoom;
        n.PinCircleRadius       = std::max(2.0f, base_.PinCircleRadius * zoom);
        n.PinQuadSideLength     = base_.PinQuadSideLength * zoom;
        n.PinTriangleSideLength = base_.PinTriangleSideLength * zoom;
        n.PinLineThickness      = std::max(1.0f, base_.PinLineThickness * zoom);
        n.PinHoverRadius        = base_.PinHoverRadius * zoom;
        n.PinOffset             = base_.PinOffset * zoom;
        changed_prev_ = zoom;
    }

    // First call inside the editor (its scrolling region is the current window).
    void inside() { ImGui::SetWindowFontScale(zoom); }

    void end() { ImGui::PopStyleVar(3); }

    // Right-drag / middle / Alt+left-drag pan, wheel zoom (around the mouse), Shift+wheel sideways, Alt+wheel up/down.
    // Returns true for a right *click* on empty canvas (for the "add block" menu).
    bool navigate(bool editor_hovered, ImVec2 origin)
    {
        changed = false;
        ImGuiIO& io  = ImGui::GetIO();
        ImVec2   pan = ImNodes::EditorContextGetPanning();
        bool     moved = false;

        int hid = 0;
        const bool on_item = ImNodes::IsNodeHovered(&hid) || ImNodes::IsLinkHovered(&hid);

        if (editor_hovered && !on_item && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            right_pan_ = true;
        if (right_pan_ && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f)) {
            pan.x += io.MouseDelta.x;
            pan.y += io.MouseDelta.y;
            moved = true;
        }
        bool click = false;
        if (right_pan_ && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 4.0f);
            click = d.x == 0.0f && d.y == 0.0f;
            right_pan_ = false;
        }

        if (editor_hovered && !io.WantTextInput) {
            const float step = ImGui::GetFontSize() * 3.0f;
            if (io.MouseWheel != 0.0f) {
                if (io.KeyShift) {
                    pan.x += io.MouseWheel * step;
                    moved = true;
                } else if (io.KeyAlt) {
                    pan.y += io.MouseWheel * step;
                    moved = true;
                } else {
                    set_zoom(zoom * std::pow(1.1f, io.MouseWheel), pan, ImVec2(io.MousePos.x - origin.x, io.MousePos.y - origin.y));
                    moved = true;
                }
            }
            if (io.MouseWheelH != 0.0f) {
                pan.x += io.MouseWheelH * step;
                moved = true;
            }
        }
        if (moved) ImNodes::EditorContextResetPanning(pan);
        return click && editor_hovered;
    }

    // Zoom buttons for a toolbar; `pivot` is the canvas centre (relative to its origin) they zoom around.
    void toolbar(ImVec2 pivot)
    {
        ImGui::SmallButton("-");
        if (ImGui::IsItemClicked()) zoom_around(zoom / 1.2f, pivot);
        ImGui::SameLine();
        ImGui::SmallButton("+");
        if (ImGui::IsItemClicked()) zoom_around(zoom * 1.2f, pivot);
        ImGui::SameLine();
        if (ImGui::SmallButton("100%")) zoom_around(1.0f, pivot);
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f%%", zoom * 100.0f);
    }

private:
    void set_zoom(float z, ImVec2& pan, ImVec2 mouse_rel)
    {
        z = std::clamp(z, 0.25f, 2.0f);
        if (z == zoom) return;
        const float f = z / zoom;
        // keep the grid point under the mouse where it is
        pan.x = mouse_rel.x - (mouse_rel.x - pan.x) * f;
        pan.y = mouse_rel.y - (mouse_rel.y - pan.y) * f;
        zoom    = z;
        changed = true;
    }
    void zoom_around(float z, ImVec2 pivot)
    {
        ImVec2 pan = ImNodes::EditorContextGetPanning();
        set_zoom(z, pan, pivot);
        ImNodes::EditorContextResetPanning(pan);
    }

    ImNodesStyle base_{};
    bool         have_base_ = false;
    bool         right_pan_ = false;
    float        changed_prev_ = 1.0f;
};

// Once after creating the context: Alt + left-drag pans like the middle button.
inline void imnodes_setup_navigation()
{
    ImNodes::GetIO().EmulateThreeButtonMouse.Modifier = &ImGui::GetIO().KeyAlt;
}
