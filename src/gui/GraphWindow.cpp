#include "GraphWindow.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <tuple>
#include <imgui.h>
#include <implot.h>

// ─── RingBuffer ─────────────────────────────────────────────────────────────

void GraphWindow::RingBuffer::push(double t, double v)
{
    if (xs.size() < CAPACITY) {
        xs.push_back(t);
        ys.push_back(v);
    } else {
        xs[head] = t;
        ys[head] = v;
        head = (head + 1) % CAPACITY;
    }
}

size_t GraphWindow::RingBuffer::lower_bound(double t) const
{
    size_t lo = 0, hi = size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (x(mid) < t) lo = mid + 1;
        else            hi = mid;
    }
    return lo;
}

// ─── data input ─────────────────────────────────────────────────────────────

GraphWindow::GraphWindow()
    : start_(std::chrono::system_clock::now())
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
                    s.data.push(t, sig.value);
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
        std::chrono::system_clock::now() - start_).count();

    std::lock_guard lock(mutex_);
    for (auto& tab : graphs_) {
        for (auto& s : tab.series) {
            if (s.interface   == interface   &&
                s.msg_id      == 0           &&
                s.signal_name == signal_name)
            {
                s.data.push(t, value);
                break;
            }
        }
    }
}

// ─── graph management ───────────────────────────────────────────────────────

size_t GraphWindow::add_graph(std::string name, double window_s, bool auto_y)
{
    std::lock_guard lock(mutex_);
    return add_graph_unlocked(std::move(name), window_s, auto_y);
}

size_t GraphWindow::add_graph_unlocked(std::string name, double window_s, bool auto_y)
{
    if (name.empty())
        name = std::format("Graph {}", graphs_.size() + 1);
    GraphTab tab;
    tab.name          = std::move(name);
    tab.x_range       = std::clamp(window_s, 0.1, 7200.0);
    tab.default_range = tab.x_range;
    tab.auto_y        = auto_y;
    graphs_.push_back(std::move(tab));
    return graphs_.size() - 1;
}

void GraphWindow::add_signal(size_t             graph_idx,
                             const std::string& interface,
                             uint32_t           msg_id,
                             const std::string& msg_name,
                             const std::string& signal_name,
                             double             y_min,
                             double             y_max,
                             int                y_axis,
                             std::optional<double> cfg_y_min,
                             std::optional<double> cfg_y_max)
{
    std::lock_guard lock(mutex_);
    if (graph_idx >= graphs_.size())
        return;
    auto& tab = graphs_[graph_idx];
    for (const auto& s : tab.series)
        if (s.interface   == interface &&
            s.msg_name    == msg_name  &&
            s.signal_name == signal_name)
            return;  // already present

    SignalSeries s;
    s.interface   = interface;
    s.msg_id      = msg_id;
    s.msg_name    = msg_name;
    s.signal_name = signal_name;
    s.label       = msg_name.empty() ? signal_name : msg_name + "/" + signal_name;
    s.y_min       = cfg_y_min.value_or(y_min);
    s.y_max       = cfg_y_max.value_or(y_max);
    s.y_axis      = std::clamp(y_axis, 0, 2);
    s.cfg_y_min   = cfg_y_min;
    s.cfg_y_max   = cfg_y_max;
    tab.series.push_back(std::move(s));
    tab.init_y = true;
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

void GraphWindow::clear()
{
    std::lock_guard lock(mutex_);
    graphs_.clear();
    pending_series_ctx_.reset();
}

std::vector<GraphConfig> GraphWindow::export_config() const
{
    std::lock_guard lock(mutex_);
    std::vector<GraphConfig> out;
    for (const auto& tab : graphs_) {
        GraphConfig gc;
        gc.name     = tab.name;
        gc.window_s = tab.default_range;
        gc.auto_y   = tab.auto_y;
        for (const auto& s : tab.series) {
            GraphSignalConfig sc;
            sc.interface = s.interface;
            sc.msg       = s.msg_name;
            sc.signal    = s.signal_name;
            sc.y_axis    = s.y_axis;
            sc.y_min     = s.cfg_y_min;
            sc.y_max     = s.cfg_y_max;
            gc.signals.push_back(std::move(sc));
        }
        out.push_back(std::move(gc));
    }
    return out;
}

// ─── view helpers ───────────────────────────────────────────────────────────

void GraphWindow::reset_view(GraphTab& tab)
{
    tab.follow  = true;
    tab.auto_y  = true;
    tab.x_range = tab.default_range;
}

void GraphWindow::fit_all(GraphTab& tab)
{
    double lo = std::numeric_limits<double>::max();
    double hi = std::numeric_limits<double>::lowest();
    for (const auto& s : tab.series) {
        if (s.data.empty()) continue;
        lo = std::min(lo, s.data.x(0));
        hi = std::max(hi, s.data.x(s.data.size() - 1));
    }
    if (lo > hi) return;
    if (hi - lo < 1e-3) { lo -= 0.5; hi += 0.5; }
    tab.follow = false;
    tab.set_x  = std::make_pair(lo, hi);
    tab.fit_y  = true;
}

// Copies the samples of s inside [x0, x1] into s.px/s.py. Long ranges are
// reduced to min/max per pixel column, which keeps peaks visible and the
// rendering cost independent of the sample rate.
// Extends y_lo/y_hi by the visible values.
static void decimate(const auto& data, double x0, double x1, int buckets,
                     std::vector<double>& px, std::vector<double>& py,
                     double& y_lo, double& y_hi)
{
    px.clear();
    py.clear();
    if (data.empty() || x1 <= x0)
        return;

    // One sample beyond each edge so lines run to the plot border.
    size_t i0 = data.lower_bound(x0);
    size_t i1 = data.lower_bound(x1);
    if (i0 > 0) --i0;
    if (i1 < data.size()) ++i1;
    if (i0 >= i1)
        return;

    auto track = [&](double x, double y) {
        if (x >= x0 && x <= x1) {
            y_lo = std::min(y_lo, y);
            y_hi = std::max(y_hi, y);
        }
    };

    const size_t n = i1 - i0;
    if (n <= static_cast<size_t>(buckets) * 2) {
        px.reserve(n);
        py.reserve(n);
        for (size_t i = i0; i < i1; ++i) {
            px.push_back(data.x(i));
            py.push_back(data.y(i));
            track(data.x(i), data.y(i));
        }
        // Nothing inside the window (sparse signal): still scale to the
        // neighbouring samples so the line is visible.
        if (y_lo > y_hi)
            for (size_t k = 0; k < py.size(); ++k) {
                y_lo = std::min(y_lo, py[k]);
                y_hi = std::max(y_hi, py[k]);
            }
        return;
    }

    const double width = (x1 - x0) / buckets;
    px.reserve(buckets * 2 + 2);
    py.reserve(buckets * 2 + 2);
    size_t i = i0;
    while (i < i1) {
        const double bucket_end = x0 + (std::floor((data.x(i) - x0) / width) + 1.0) * width;
        size_t i_min = i, i_max = i;
        size_t j = i;
        for (; j < i1 && data.x(j) < bucket_end; ++j) {
            if (data.y(j) < data.y(i_min)) i_min = j;
            if (data.y(j) > data.y(i_max)) i_max = j;
        }
        if (j == i) ++j;  // guard against NaN timestamps
        const size_t a = std::min(i_min, i_max);
        const size_t b = std::max(i_min, i_max);
        px.push_back(data.x(a)); py.push_back(data.y(a)); track(data.x(a), data.y(a));
        if (b != a) {
            px.push_back(data.x(b)); py.push_back(data.y(b)); track(data.x(b), data.y(b));
        }
        i = j;
    }
}

// ─── rendering ──────────────────────────────────────────────────────────────

static const std::array<std::pair<const char*, double>, 10> WINDOW_PRESETS = {{
    {"1 s", 1}, {"2 s", 2}, {"5 s", 5}, {"10 s", 10}, {"30 s", 30},
    {"1 min", 60}, {"2 min", 120}, {"5 min", 300}, {"10 min", 600}, {"30 min", 1800},
}};

static std::string format_window(double s)
{
    if (s >= 60.0) return std::format("{:.3g} min", s / 60.0);
    return std::format("{:.3g} s", s);
}

void GraphWindow::render_toolbar(GraphTab& tab)
{
    // Follow / pause toggle, colored so the state is obvious at a glance.
    const ImVec4 follow_col = tab.follow ? ImVec4(0.20f, 0.60f, 0.25f, 1.0f)
                                         : ImVec4(0.80f, 0.50f, 0.10f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button,        follow_col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(follow_col.x + 0.1f, follow_col.y + 0.1f, follow_col.z + 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  follow_col);
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1, 1, 1, 1));
    if (ImGui::Button(tab.follow ? "Live" : "Paused", ImVec2(ImGui::GetFontSize() * 5.0f, 0)))
        tab.follow = !tab.follow;
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(tab.follow ? "Following the newest data - click to pause"
                                     : "View is frozen - click to follow the newest data again");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    const std::string current = format_window(tab.x_range);
    if (ImGui::BeginCombo("##window", current.c_str())) {
        for (const auto& [label, secs] : WINDOW_PRESETS) {
            if (ImGui::Selectable(label, std::abs(tab.x_range - secs) < 1e-9)) {
                tab.x_range = secs;
                tab.follow  = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Visible time window (also: mouse wheel in the plot)");

    ImGui::SameLine();
    ImGui::Checkbox("Auto Y", &tab.auto_y);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scale the Y axes to the visible data.\n"
                          "Zooming/dragging a Y axis switches to manual.");

    ImGui::SameLine();
    if (ImGui::Button("Fit"))
        fit_all(tab);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show all recorded data");

    ImGui::SameLine();
    if (ImGui::Button("Default"))
        reset_view(tab);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Back to the default view: live, %s window, Auto Y\n(also: double-click the plot)",
                          format_window(tab.default_range).c_str());

    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        for (auto& s : tab.series) s.data.clear();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Discard the recorded samples of this graph");

    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Mouse wheel        zoom time window\n"
            "Left drag          pan (pauses)\n"
            "Right drag         zoom to rectangle (pauses)\n"
            "Wheel on Y axis    zoom Y (switches Auto Y off)\n"
            "Double-click       default view\n"
            "Right-click legend change axis / remove signal\n"
            "Right-click tab    rename / delete graph");

    size_t unresolved = 0;
    for (const auto& s : tab.series)
        if (s.msg_id == UNRESOLVED) ++unresolved;
    if (unresolved > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.1f, 1.0f), "%zu signal(s) not found in DBC", unresolved);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (const auto& s : tab.series)
                if (s.msg_id == UNRESOLVED)
                    ImGui::Text("%s: %s", s.interface.empty() ? "?" : s.interface.c_str(), s.label.c_str());
            ImGui::EndTooltip();
        }
    }
}

void GraphWindow::render_plot(size_t ti, GraphTab& tab)
{
    ImGuiIO& io = ImGui::GetIO();

    // ── X range for this frame ──
    double t_max = 0.0;
    bool   any   = false;
    for (const auto& s : tab.series)
        if (!s.data.empty()) {
            t_max = any ? std::max(t_max, s.data.x(s.data.size() - 1)) : s.data.x(s.data.size() - 1);
            any   = true;
        }

    double x0 = tab.view_x0, x1 = tab.view_x1;
    if (tab.set_x) {
        std::tie(x0, x1) = *tab.set_x;
    } else if (tab.follow) {
        // Until the window is full, keep t=0 at the left edge.
        x1 = std::max(t_max, tab.x_range);
        x0 = x1 - tab.x_range;
    }

    // ── decimate visible data, collect Y ranges per axis ──
    const int buckets = std::max(64, static_cast<int>(ImGui::GetContentRegionAvail().x));
    std::array<double, 3> y_lo, y_hi;
    y_lo.fill(std::numeric_limits<double>::max());
    y_hi.fill(std::numeric_limits<double>::lowest());
    bool has_y[3] = { true, false, false };
    for (auto& s : tab.series) {
        has_y[s.y_axis] = true;
        decimate(s.data, x0, x1, buckets, s.px, s.py, y_lo[s.y_axis], y_hi[s.y_axis]);
    }

    // While following, the mouse wheel inside the plot changes the time
    // window instead of letting ImPlot zoom both axes.
    const ImVec2 mouse = io.MousePos;
    const bool   wheel_in_plot = tab.follow && io.MouseWheel != 0.0f &&
                                 ImGui::IsWindowHovered() &&
                                 mouse.x >= tab.plot_rect[0] && mouse.y >= tab.plot_rect[1] &&
                                 mouse.x <= tab.plot_rect[2] && mouse.y <= tab.plot_rect[3];
    float saved_wheel = 0.0f;
    if (wheel_in_plot) {
        tab.x_range = std::clamp(tab.x_range * std::pow(0.85, static_cast<double>(io.MouseWheel)), 0.1, 7200.0);
        saved_wheel   = io.MouseWheel;
        io.MouseWheel = 0.0f;
    }

    const std::string plot_id = std::format("##plot{}", ti);
    if (ImPlot::BeginPlot(plot_id.c_str(), ImVec2(-1, -1), ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("Time (s)", nullptr);
        if (has_y[1]) ImPlot::SetupAxis(ImAxis_Y2, nullptr, ImPlotAxisFlags_AuxDefault);
        if (has_y[2]) ImPlot::SetupAxis(ImAxis_Y3, nullptr, ImPlotAxisFlags_AuxDefault);
        ImPlot::SetupLegend(ImPlotLocation_NorthWest);

        if (tab.follow || tab.set_x)
            ImPlot::SetupAxisLimits(ImAxis_X1, x0, x1, ImGuiCond_Always);
        tab.set_x.reset();

        for (int a = 0; a < 3; ++a) {
            if (!has_y[a]) continue;
            const ImAxis axis = ImAxis_Y1 + a;
            if (tab.auto_y || tab.fit_y) {
                double lo = y_lo[a], hi = y_hi[a];
                if (lo > hi) continue;                 // no data on this axis
                const double span = hi - lo;
                const double pad  = span > 0.0 ? span * 0.06 : std::max(std::abs(hi) * 0.1, 1.0);
                ImPlot::SetupAxisLimits(axis, lo - pad, hi + pad, ImGuiCond_Always);
            } else if (tab.init_y) {
                // Manual mode: start from the DBC ranges.
                double lo = std::numeric_limits<double>::max();
                double hi = std::numeric_limits<double>::lowest();
                for (const auto& s : tab.series) {
                    if (s.y_axis != a || (s.y_min == 0.0 && s.y_max == 0.0)) continue;
                    lo = std::min(lo, s.y_min);
                    hi = std::max(hi, s.y_max);
                }
                if (lo < hi)
                    ImPlot::SetupAxisLimits(axis, lo, hi, ImGuiCond_Always);
            }
        }
        tab.init_y = false;
        tab.fit_y  = false;

        // ── interaction ──
        if (ImPlot::IsPlotHovered() && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                                        ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
            tab.drag_in_plot = true;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
            tab.drag_in_plot = false;
        if (tab.drag_in_plot && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
                                 ImGui::IsMouseDragging(ImGuiMouseButton_Right)))
            tab.follow = false;

        if (ImPlot::IsPlotHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            reset_view(tab);

        if (tab.follow && ImPlot::IsAxisHovered(ImAxis_X1) && io.MouseWheel != 0.0f)
            tab.x_range = std::clamp(tab.x_range * std::pow(0.85, static_cast<double>(io.MouseWheel)), 0.1, 7200.0);

        for (int a = 0; a < 3; ++a) {
            if (!has_y[a]) continue;
            if (ImPlot::IsAxisHovered(ImAxis_Y1 + a) &&
                (saved_wheel != 0.0f || io.MouseWheel != 0.0f || ImGui::IsMouseDragging(ImGuiMouseButton_Left)))
                tab.auto_y = false;
        }

        // ── series ──
        for (size_t si = 0; si < tab.series.size(); ++si) {
            auto& s = tab.series[si];
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1 + s.y_axis);
            ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, 2.0f);
            ImPlot::PlotLine(s.label.c_str(), s.px.data(), s.py.data(), static_cast<int>(s.px.size()));
            s.color = ImGui::ColorConvertFloat4ToU32(ImPlot::GetLastItemColor());
            if (ImPlot::IsLegendEntryHovered(s.label.c_str()) &&
                ImGui::IsMouseReleased(ImGuiMouseButton_Right))
            {
                pending_series_ctx_ = { ti, si, s.label, s.y_axis };
                open_series_ctx_    = true;
            }
        }
        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);

        // ── cursor read-out ──
        if (ImPlot::IsPlotHovered() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
            !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !tab.series.empty())
        {
            const double t = ImPlot::GetPlotMousePos(ImAxis_X1, ImAxis_Y1).x;
            ImDrawList* dl = ImPlot::GetPlotDrawList();
            ImPlot::PushPlotClipRect();
            const float px = ImPlot::PlotToPixels(t, 0.0, ImAxis_X1, ImAxis_Y1).x;
            dl->AddLine(ImVec2(px, ImPlot::GetPlotPos().y),
                        ImVec2(px, ImPlot::GetPlotPos().y + ImPlot::GetPlotSize().y),
                        IM_COL32(128, 128, 128, 160));
            ImPlot::PopPlotClipRect();

            ImGui::BeginTooltip();
            ImGui::Text("t = %.3f s", t);
            ImGui::Separator();
            for (const auto& s : tab.series) {
                if (s.data.empty()) {
                    ImGui::TextDisabled("%s: -", s.label.c_str());
                    continue;
                }
                size_t i = s.data.lower_bound(t);
                if (i >= s.data.size()) i = s.data.size() - 1;
                if (i > 0 && std::abs(s.data.x(i - 1) - t) < std::abs(s.data.x(i) - t)) --i;
                ImGui::PushStyleColor(ImGuiCol_Text, s.color);
                ImGui::Bullet();
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::Text("%s: %.6g", s.label.c_str(), s.data.y(i));
            }
            ImGui::EndTooltip();
        }

        const ImPlotRect lim = ImPlot::GetPlotLimits(ImAxis_X1, ImAxis_Y1);
        tab.view_x0  = lim.X.Min;
        tab.view_x1  = lim.X.Max;
        const ImVec2 pos  = ImPlot::GetPlotPos();
        const ImVec2 size = ImPlot::GetPlotSize();
        tab.plot_rect[0] = pos.x;
        tab.plot_rect[1] = pos.y;
        tab.plot_rect[2] = pos.x + size.x;
        tab.plot_rect[3] = pos.y + size.y;
        ImPlot::EndPlot();
    }

    if (saved_wheel != 0.0f)
        io.MouseWheel = saved_wheel;
}

void GraphWindow::render_series_popup()
{
    if (open_series_ctx_) {
        ImGui::OpenPopup("##series_ctx");
        open_series_ctx_ = false;
    }
    if (!ImGui::BeginPopup("##series_ctx"))
        return;
    if (pending_series_ctx_) {
        ImGui::Text("%s", pending_series_ctx_->label.c_str());
        ImGui::Separator();
        const char* axis_labels[] = { "Y1 (left)", "Y2 (right)", "Y3 (right)" };
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
        const bool do_clear  = ImGui::MenuItem("Clear data");
        const bool do_remove = ImGui::MenuItem("Remove");

        if (changed_axis || do_remove || do_clear) {
            const size_t ti = pending_series_ctx_->tab_idx;
            const size_t si = pending_series_ctx_->series_idx;
            if (ti < graphs_.size() && si < graphs_[ti].series.size()) {
                auto& tab = graphs_[ti];
                if (do_remove)
                    tab.series.erase(tab.series.begin() + si);
                else if (do_clear)
                    tab.series[si].data.clear();
                else
                    tab.series[si].y_axis = new_axis;
                tab.init_y = true;
            }
            pending_series_ctx_.reset();
        }
    }
    ImGui::EndPopup();
}

void GraphWindow::render()
{
    if (!ImGui::Begin(title(), &visible)) {
        ImGui::End();
        return;
    }

    std::lock_guard lock(mutex_);

    if (graphs_.empty()) {
        ImGui::TextDisabled("No graphs yet.");
        if (ImGui::Button("+ Add Graph"))
            add_graph_unlocked({}, DEFAULT_WINDOW_S, true);
        ImGui::TextDisabled("Then right-click a signal in the Trace window to plot it.");
        ImGui::End();
        return;
    }

    bool add_requested = false;
    if (ImGui::BeginTabBar("##graphs", ImGuiTabBarFlags_Reorderable |
                                       ImGuiTabBarFlags_AutoSelectNewTabs |
                                       ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (size_t ti = 0; ti < graphs_.size(); ++ti) {
            auto& tab = graphs_[ti];
            const std::string tab_label = std::format("{}###graph{}", tab.name, ti);
            const bool open = ImGui::BeginTabItem(tab_label.c_str());

            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Rename...")) {
                    rename_idx_ = ti;
                    std::snprintf(rename_buf_, sizeof(rename_buf_), "%s", tab.name.c_str());
                }
                if (ImGui::MenuItem("Clear data"))
                    for (auto& s : tab.series) s.data.clear();
                ImGui::Separator();
                if (ImGui::MenuItem("Delete graph"))
                    delete_idx_ = ti;
                ImGui::EndPopup();
            }

            if (!open)
                continue;

            render_toolbar(tab);
            if (tab.series.empty()) {
                ImGui::Spacing();
                ImGui::TextDisabled("Empty graph - right-click a signal in the Trace window and choose \"%s\".",
                                    tab.name.c_str());
            } else {
                render_plot(ti, tab);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
            add_requested = true;
        ImGui::EndTabBar();
    }

    render_series_popup();

    // Rename dialog
    if (rename_idx_) {
        ImGui::OpenPopup("Rename graph");
    }
    if (ImGui::BeginPopupModal("Rename graph", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##name", rename_buf_, sizeof(rename_buf_),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::Button("OK")) {
            if (rename_idx_ && *rename_idx_ < graphs_.size() && rename_buf_[0] != '\0')
                graphs_[*rename_idx_].name = rename_buf_;
            rename_idx_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            rename_idx_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (delete_idx_) {
        if (*delete_idx_ < graphs_.size())
            graphs_.erase(graphs_.begin() + static_cast<std::ptrdiff_t>(*delete_idx_));
        delete_idx_.reset();
        pending_series_ctx_.reset();
    }

    if (add_requested)
        add_graph_unlocked({}, DEFAULT_WINDOW_S, true);

    ImGui::End();
}
