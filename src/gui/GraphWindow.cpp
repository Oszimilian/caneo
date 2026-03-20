#include "GraphWindow.hpp"

#include <format>
#include <limits>
#include <imgui.h>
#include <implot.h>

GraphWindow::GraphWindow()
    : start_(std::chrono::steady_clock::now())
{}

void GraphWindow::update(const CanFrame& frame)
{
    const double t = std::chrono::duration<double>(
        frame.timestamp() - start_).count();

    std::lock_guard lock(mutex_);
    for (auto& tab : graphs_) {
        for (auto& s : tab.series) {
            if (s.interface != frame.header().interface ||
                s.msg_id    != frame.header().id)
                continue;
            for (const auto& sig : frame.decoded()) {
                if (sig.name == s.signal_name) {
                    s.push(t, sig.value);
                    break;
                }
            }
        }
    }
}

size_t GraphWindow::add_graph()
{
    std::lock_guard lock(mutex_);
    graphs_.push_back({ std::format("graph{}", graphs_.size() + 1), {} });
    return graphs_.size() - 1;
}

void GraphWindow::add_signal(size_t             graph_idx,
                             const std::string& interface,
                             uint32_t           msg_id,
                             const std::string& signal_name,
                             double             y_min,
                             double             y_max)
{
    std::lock_guard lock(mutex_);
    if (graph_idx >= graphs_.size())
        return;
    auto& tab = graphs_[graph_idx];
    for (const auto& s : tab.series)
        if (s.interface == interface &&
            s.msg_id    == msg_id    &&
            s.signal_name == signal_name)
            return;  // already present
    tab.series.push_back({ interface, msg_id, signal_name, y_min, y_max, {}, {} });
}

std::vector<std::string> GraphWindow::graph_names() const
{
    std::lock_guard lock(mutex_);
    std::vector<std::string> names;
    names.reserve(graphs_.size());
    for (const auto& g : graphs_)
        names.push_back(g.name);
    return names;
}

void GraphWindow::render()
{
    ImGui::Begin("Graphs");

    if (ImGui::Button("+ Add Graph"))
        add_graph();

    if (graphs_.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("No graphs yet — add one, then right-click a signal in the Trace window.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##graphs")) {
        std::lock_guard lock(mutex_);
        for (auto& tab : graphs_) {
            if (!ImGui::BeginTabItem(tab.name.c_str()))
                continue;

            if (!tab.follow) {
                if (ImGui::Button("Jump to latest"))
                    tab.follow = true;
            } else {
                ImGui::TextDisabled("Following latest  —  scroll to zoom");
            }

            if (ImPlot::BeginPlot(tab.name.c_str(), ImVec2(-1, -1))) {
                ImPlot::SetupAxes("Time (s)", nullptr);

                // X: force limits only while following
                if (tab.follow) {
                    double t_max = -1.0;
                    for (const auto& s : tab.series)
                        if (!s.xs.empty() && s.xs.back() > t_max)
                            t_max = s.xs.back();
                    if (t_max >= 0.0)
                        ImPlot::SetupAxisLimits(ImAxis_X1,
                                                t_max - 30.0, t_max,
                                                ImGuiCond_Always);
                }

                // Y: union of all series' DBC min/max (skip if both are 0)
                double y_min = std::numeric_limits<double>::max();
                double y_max = std::numeric_limits<double>::lowest();
                for (const auto& s : tab.series) {
                    if (s.y_min == 0.0 && s.y_max == 0.0)
                        continue;
                    y_min = std::min(y_min, s.y_min);
                    y_max = std::max(y_max, s.y_max);
                }
                if (y_min < y_max)
                    ImPlot::SetupAxisLimits(ImAxis_Y1, y_min, y_max,
                                            ImGuiCond_Once);

                // Auto-disable follow when user scrolls or pans
                // Check both plot area and X-axis label area
                const bool hovered = ImPlot::IsPlotHovered()    ||
                                     ImPlot::IsAxisHovered(ImAxis_X1);
                if (tab.follow && hovered &&
                    (ImGui::GetIO().MouseWheel != 0.0f ||
                     ImGui::IsMouseDragging(ImGuiMouseButton_Right)))
                    tab.follow = false;

                for (auto& s : tab.series) {
                    if (s.xs.empty())
                        continue;
                    ImPlot::PlotLine(s.signal_name.c_str(),
                                     s.xs.data(), s.ys.data(),
                                     static_cast<int>(s.xs.size()));
                }
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}
