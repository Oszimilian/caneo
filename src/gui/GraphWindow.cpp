#include "GraphWindow.hpp"

#include <cmath>
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

void GraphWindow::push_value(const std::string& interface,
                             const std::string& signal_name,
                             double             value)
{
    const double t = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start_).count();

    std::lock_guard lock(mutex_);
    for (auto& tab : graphs_) {
        for (auto& s : tab.series) {
            if (s.interface   == interface   &&
                s.msg_id      == 0           &&
                s.signal_name == signal_name)
            {
                s.push(t, value);
                break;
            }
        }
    }
}

size_t GraphWindow::add_graph(std::string name)
{
    std::lock_guard lock(mutex_);
    if (name.empty())
        name = std::format("graph{}", graphs_.size() + 1);
    graphs_.push_back({ std::move(name), {} });
    return graphs_.size() - 1;
}

void GraphWindow::add_signal(size_t             graph_idx,
                             const std::string& interface,
                             uint32_t           msg_id,
                             const std::string& signal_name,
                             double             y_min,
                             double             y_max,
                             int                y_axis,
                             std::string        label)
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
    if (label.empty())
        label = signal_name;
    tab.series.push_back({ interface, msg_id, signal_name, std::move(label), y_min, y_max, y_axis, {}, {} });
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
        for (size_t ti = 0; ti < graphs_.size(); ++ti) {
            auto& tab = graphs_[ti];
            if (!ImGui::BeginTabItem(tab.name.c_str()))
                continue;

            if (ImGui::Button("Reset View"))
                tab.follow = true;
            ImGui::SameLine();
            if (tab.follow)
                ImGui::TextDisabled("Following latest  —  drag to pause");
            else
                ImGui::TextDisabled("Paused  —  press Reset View to follow again");

            if (ImPlot::BeginPlot(tab.name.c_str(), ImVec2(-1, -1))) {
                ImPlot::SetupAxes("Time (s)", nullptr);

                // Enable auxiliary Y-axes only if any series uses them
                bool has_y[3] = { false, false, false };
                for (const auto& s : tab.series)
                    if (s.y_axis >= 0 && s.y_axis < 3)
                        has_y[s.y_axis] = true;
                if (has_y[1]) ImPlot::SetupAxis(ImAxis_Y2, nullptr, ImPlotAxisFlags_AuxDefault);
                if (has_y[2]) ImPlot::SetupAxis(ImAxis_Y3, nullptr, ImPlotAxisFlags_AuxDefault);

                // X: pin right edge to latest data, window width = tab.x_range
                if (tab.follow) {
                    double t_max = -1.0;
                    for (const auto& s : tab.series)
                        if (!s.xs.empty() && s.xs.back() > t_max)
                            t_max = s.xs.back();
                    if (t_max >= 0.0)
                        ImPlot::SetupAxisLimits(ImAxis_X1,
                                                t_max - tab.x_range, t_max,
                                                ImGuiCond_Always);
                }

                // Y: initial limits per axis from DBC min/max
                for (int a = 0; a < 3; ++a) {
                    if (a > 0 && !has_y[a]) continue;
                    double y_lo = std::numeric_limits<double>::max();
                    double y_hi = std::numeric_limits<double>::lowest();
                    for (const auto& s : tab.series) {
                        if (s.y_axis != a || (s.y_min == 0.0 && s.y_max == 0.0))
                            continue;
                        y_lo = std::min(y_lo, s.y_min);
                        y_hi = std::max(y_hi, s.y_max);
                    }
                    if (y_lo < y_hi)
                        ImPlot::SetupAxisLimits(ImAxis_Y1 + a, y_lo, y_hi, ImGuiCond_Once);
                }

                // While following:
                //   X-axis scroll → zoom (update x_range), keep following
                //   Right-drag    → pan, disable follow
                if (tab.follow) {
                    const float wheel = ImGui::GetIO().MouseWheel;
                    if (ImPlot::IsAxisHovered(ImAxis_X1) && wheel != 0.0f) {
                        tab.x_range *= std::pow(0.85, (double)wheel);
                        tab.x_range  = std::clamp(tab.x_range, 0.5, 7200.0);
                    }
                    if ((ImPlot::IsPlotHovered() || ImPlot::IsAxisHovered(ImAxis_X1)) &&
                        ImGui::IsMouseDragging(ImGuiMouseButton_Right))
                        tab.follow = false;
                }

                for (size_t si = 0; si < tab.series.size(); ++si) {
                    auto& s = tab.series[si];
                    if (s.xs.empty())
                        continue;
                    const int axis = std::clamp(s.y_axis, 0, 2);
                    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1 + axis);
                    ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, 3.0f);
                    ImPlot::PlotLine(s.label.c_str(),
                                     s.xs.data(), s.ys.data(),
                                     static_cast<int>(s.xs.size()));
                    if (ImPlot::IsLegendEntryHovered(s.label.c_str()) &&
                        ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                    {
                        pending_series_ctx_ = { ti, si, s.signal_name, s.y_axis };
                        open_series_ctx_    = true;
                    }
                }
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // Right-click context menu for series (axis change / remove)
    if (open_series_ctx_) {
        ImGui::OpenPopup("##series_ctx");
        open_series_ctx_ = false;
    }
    if (ImGui::BeginPopup("##series_ctx")) {
        if (pending_series_ctx_) {
            ImGui::Text("Signal: %s", pending_series_ctx_->signal_name.c_str());
            ImGui::Separator();
            const char* axis_labels[] = { "Y1", "Y2", "Y3" };
            bool changed_axis = false;
            int  new_axis     = pending_series_ctx_->current_axis;
            for (int a = 0; a < 3; ++a) {
                const bool active = (pending_series_ctx_->current_axis == a);
                if (ImGui::MenuItem(axis_labels[a], nullptr, active) && !active) {
                    new_axis     = a;
                    changed_axis = true;
                }
            }
            ImGui::Separator();
            bool do_remove = ImGui::MenuItem("Remove");

            if (changed_axis || do_remove) {
                std::lock_guard lock(mutex_);
                const size_t ti = pending_series_ctx_->tab_idx;
                const size_t si = pending_series_ctx_->series_idx;
                if (ti < graphs_.size() && si < graphs_[ti].series.size()) {
                    if (do_remove)
                        graphs_[ti].series.erase(graphs_[ti].series.begin() + si);
                    else
                        graphs_[ti].series[si].y_axis = new_axis;
                }
                pending_series_ctx_.reset();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}
