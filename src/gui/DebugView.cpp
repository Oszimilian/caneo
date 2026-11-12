#include "DebugView.hpp"

#include "AppState.hpp"
#include "debug/CsvExport.hpp"
#include "debug/OfflineFlow.hpp"
#include "flow/Flow.hpp"

#include <functional>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <implot.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <format>
#include <optional>
#include <unordered_map>

namespace {

bool contains_nocase(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    return std::ranges::search(hay, needle, [](unsigned char a, unsigned char b) {
               return std::tolower(a) == std::tolower(b);
           }).begin() != hay.end();
}

const char* format_name(LogDocument::Format f)
{
    switch (f) {
        case LogDocument::Format::Mcap: return "MCAP";
        case LogDocument::Format::Blf:  return "BLF";
        default:                        return "";
    }
}

// Flow editor backend for the offline flows: the specs live in config().debug_flows,
// every change re-runs them over the loaded log.
class OfflineFlowBackend : public FlowBackend {
public:
    OfflineFlowBackend(std::vector<FlowSpec>& specs, std::vector<std::shared_ptr<FlowState>>& states,
                       const std::vector<std::vector<CsvTable>>& csv, const LogDocument& doc,
                       const unsigned& version, std::function<void()> changed)
        : specs_(specs), states_(states), csv_(csv), doc_(doc), version_(version), changed_(std::move(changed)) {}

    std::string export_csv(std::size_t flow, int node_id, const std::string& path) override
    {
        if (path.empty()) return "Set a file path first";
        if (flow >= csv_.size()) return "No data - apply the flow first";
        for (const auto& t : csv_[flow]) {
            if (t.node_id != node_id) continue;
            std::size_t rows = 0;
            const auto  err  = write_table_csv(expand_path(path), t, rows);
            return err.empty() ? std::format("Wrote {} rows to {}", rows, path) : err;
        }
        return "No data - apply the flow first";
    }

    bool offline() const override { return true; }

    std::vector<FlowInfo> snapshot() const override
    {
        sync();
        std::vector<FlowInfo> out;
        for (std::size_t i = 0; i < specs_.size(); ++i)
            out.push_back({i, specs_[i], states_[i]->log.error().empty(), specs_[i].enabled, states_[i]});
        return out;
    }
    void add(const FlowSpec& s) override { specs_.push_back(s); sync(); changed_(); }
    void replace(std::size_t i, const FlowSpec& s) override
    {
        if (i < specs_.size()) { specs_[i] = s; changed_(); }
    }
    void remove(std::size_t i) override
    {
        if (i >= specs_.size()) return;
        specs_.erase(specs_.begin() + static_cast<std::ptrdiff_t>(i));
        sync();
        changed_();
    }
    void set_enabled(std::size_t i, bool on) override
    {
        if (i < specs_.size()) { specs_[i].enabled = on; changed_(); }
    }

    std::vector<std::string> interfaces() const override { return doc_.data().interfaces; }

    const std::vector<Msg>& messages(const std::string& iface) override
    {
        if (cache_version_ != version_) {   // new log / other DBCs
            cache_.clear();
            const auto& d = doc_.data();
            for (std::size_t i = 0; i < d.base_series && i < d.series.size(); ++i) {
                const auto& s = d.series[i];
                auto&       list = cache_[s.iface];
                if (list.empty() || list.back().name != s.msg) list.push_back({s.msg, {}});
                list.back().signals.push_back(s.signal);
            }
            cache_version_ = version_;
        }
        static const std::vector<Msg> none;
        const auto it = cache_.find(iface);
        return it == cache_.end() ? none : it->second;
    }

private:
    void sync() const
    {
        while (states_.size() < specs_.size()) states_.push_back(std::make_shared<FlowState>());
        states_.resize(specs_.size());
    }

    std::vector<FlowSpec>&                        specs_;
    std::vector<std::shared_ptr<FlowState>>&      states_;
    const std::vector<std::vector<CsvTable>>&     csv_;
    const LogDocument&                            doc_;
    const unsigned&                               version_;
    std::function<void()>                         changed_;
    unsigned                                      cache_version_ = ~0u;
    std::map<std::string, std::vector<Msg>>       cache_;
};

// Amplitude spectrum of a series between ta and tb: resampled linearly to a uniform grid
// (rate = median sample interval), mean removed, Hann window, zero padded to 2^n.
std::optional<std::pair<std::vector<double>, std::vector<double>>>
spectrum_of(const LogDocument::Series& s, double ta, double tb, double& fs_out, std::size_t& n_out, std::size_t& nfft_out)
{
    constexpr std::size_t MAX_N = 1u << 17;
    const std::size_t lo = std::lower_bound(s.t.begin(), s.t.end(), ta) - s.t.begin();
    const std::size_t hi = std::upper_bound(s.t.begin(), s.t.end(), tb) - s.t.begin();
    if (hi < lo + 8) return std::nullopt;

    std::vector<double> diffs;
    diffs.reserve(hi - lo - 1);
    for (std::size_t i = lo + 1; i < hi; ++i)
        if (s.t[i] > s.t[i - 1]) diffs.push_back(s.t[i] - s.t[i - 1]);
    if (diffs.empty()) return std::nullopt;
    std::nth_element(diffs.begin(), diffs.begin() + static_cast<std::ptrdiff_t>(diffs.size() / 2), diffs.end());
    double dt = diffs[diffs.size() / 2];

    const double t0 = s.t[lo], t1 = s.t[hi - 1];
    if (t1 <= t0) return std::nullopt;
    dt = std::max(dt, (t1 - t0) / static_cast<double>(MAX_N - 1));
    const auto n = static_cast<std::size_t>((t1 - t0) / dt) + 1;
    if (n < 8) return std::nullopt;

    std::vector<double> x(n);
    std::size_t j = lo;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = t0 + static_cast<double>(i) * dt;
        while (j + 1 < hi && s.t[j + 1] <= t) ++j;
        if (j + 1 < hi) {
            const double span = s.t[j + 1] - s.t[j];
            const double u    = span > 0.0 ? (t - s.t[j]) / span : 0.0;
            x[i]              = s.v[j] + (s.v[j + 1] - s.v[j]) * std::clamp(u, 0.0, 1.0);
        } else {
            x[i] = s.v[j];
        }
    }
    double mean = 0.0;
    for (const double v : x) mean += v;
    mean /= static_cast<double>(n);

    std::size_t nfft = 8;
    while (nfft < n) nfft <<= 1;
    std::vector<std::complex<double>> a(nfft);
    double wsum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n - 1));
        a[i]  = (x[i] - mean) * w;
        wsum += w;
    }
    fft_inplace(a);

    const double fs = 1.0 / dt;
    std::vector<double> f(nfft / 2), amp(nfft / 2);
    for (std::size_t k = 0; k < nfft / 2; ++k) {
        f[k]   = static_cast<double>(k) * fs / static_cast<double>(nfft);
        amp[k] = 2.0 * std::abs(a[k]) / wsum;
    }
    fs_out = fs; n_out = n; nfft_out = nfft;
    return std::make_pair(std::move(f), std::move(amp));
}

} // namespace

DebugView::DebugView(AppState& state) : state_(state)
{
    state_.set_debug_snapshot([this] { return snapshot(); });
}

DebugView::~DebugView()
{
    state_.set_debug_snapshot({});
}

std::optional<DebugSession> DebugView::snapshot() const
{
    if (!doc_.loaded()) return std::nullopt;
    const auto& d = doc_.data();

    DebugSession s;
    s.name = std::filesystem::path(d.path).filename().string();
    s.log  = d.path;
    for (const auto& iface : d.interfaces)
        if (const auto it = dbcs_.find(iface); it != dbcs_.end() && !it->second.empty())
            s.dbcs[iface] = it->second;
    for (const auto& t : tabs_) {
        DebugTabConfig tc;
        tc.name = t.name;
        tc.xy   = t.xy;
        for (const auto& p : t.plots) {
            std::vector<std::string> labels;
            for (const auto si : p.series)
                if (si < d.series.size()) labels.push_back(d.series[si].label);
            tc.plots.push_back(std::move(labels));
        }
        s.tabs.push_back(std::move(tc));
    }
    return s;
}

void DebugView::restore(const DebugSession& session)
{
    if (doc_.busy()) return;
    for (const auto& [iface, dbc] : session.dbcs)
        dbcs_[iface] = dbc;
    load(session.log);
    restore_ = session;   // after load(), which clears it
}

void DebugView::render_history_items()
{
    auto& hist = state_.config().debug_history;
    if (hist.empty()) {
        ImGui::TextDisabled("Empty - sessions are stored when the config is saved");
        return;
    }
    std::optional<std::size_t> remove;
    for (std::size_t i = 0; i < hist.size(); ++i) {
        const auto& h = hist[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string label = h.name + "   " + h.saved;
        if (ImGui::MenuItem(label.c_str(), nullptr, false, !doc_.busy()))
            restore(h);   // copies
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nRight-click: remove from history", h.log.c_str());
        if (ImGui::BeginPopupContextItem("##hctx")) {
            if (ImGui::MenuItem("Remove from history")) remove = i;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (remove)
        hist.erase(hist.begin() + static_cast<std::ptrdiff_t>(*remove));
}

std::ostream& operator<<(std::ostream& os, const DebugView& view)
{
    return os << "DebugView(" << view.doc_ << ", " << view.tabs_.size() << " tabs)";
}

// ─── data ───────────────────────────────────────────────────────────────────

void DebugView::load(const std::string& path)
{
    state_.record_debug_session();   // keep the log that is replaced, with its plots
    restore_.reset();
    log_path_ = path;
    std::vector<std::string> names;
    for (const auto& cfg : state_.config().interfaces) {
        if (!cfg.dbc.empty() && !dbcs_.contains(cfg.name))
            dbcs_[cfg.name] = cfg.dbc;
    }
    // BLF channel n is "can<n>" unless blf_channels names it (config order of the interfaces is not used).
    for (const auto& [ch, name] : state_.config().blf_channels) {
        if (ch < 1) continue;
        while (names.size() < static_cast<std::size_t>(ch))
            names.push_back(std::format("can{}", names.size() + 1));
        names[static_cast<std::size_t>(ch) - 1] = name;
    }
    doc_.open(path, names, dbcs_);
}

void DebugView::on_data_changed()
{
    const auto& d = doc_.data();

    // DBC defaults from the config for interfaces seen the first time
    for (const auto& iface : d.interfaces) {
        if (dbcs_.contains(iface)) continue;
        for (const auto& cfg : state_.config().interfaces)
            if (cfg.name == iface) dbcs_[iface] = cfg.dbc;
    }

    ++data_version_;
    ++xy_epoch_;

    // series indices of the old document are meaningless now
    for (auto& t : tabs_) {
        for (auto& p : t.plots) p.series.clear();
        t.fit_requested = true;
    }
    // the open DBC viewer still shows a valid file, keep it

    apply_flows(false);   // also builds the signal tree

    if (restore_) {
        std::unordered_map<std::string, std::size_t> by_label;
        for (std::size_t i = 0; i < d.series.size(); ++i)
            by_label[d.series[i].label] = i;
        tabs_.clear();
        for (const auto& tc : restore_->tabs) {
            PlotTab tab;
            tab.name = tc.name;
            tab.xy   = tc.xy;
            tab.plots.clear();
            for (const auto& labels : tc.plots) {
                Plot p;
                for (const auto& l : labels)
                    if (const auto it = by_label.find(l); it != by_label.end()) p.series.push_back(it->second);
                tab.plots.push_back(std::move(p));
            }
            if (tab.plots.empty()) tab.plots.emplace_back();
            tabs_.push_back(std::move(tab));
        }
        if (tabs_.empty()) tabs_.push_back(PlotTab{"Plot 1"});
        current_tab_ = 0;
        tab_counter_ = static_cast<int>(tabs_.size());
        restore_.reset();
    }
}

void DebugView::rebuild_tree()
{
    const auto& d = doc_.data();
    tree_.clear();
    for (std::size_t i = 0; i < d.series.size(); ++i)
        tree_[d.series[i].iface][d.series[i].msg].push_back(i);
    for (const auto& u : d.unmatched)
        tree_[u.iface];   // interfaces that only have unmatched IDs (e.g. no DBC) appear too

}

void DebugView::apply_flows(bool keep_plots)
{
    // labels of the plotted series: the indices of flow results change
    std::vector<std::vector<std::vector<std::string>>> labels;
    if (keep_plots) {
        const auto& d = doc_.data();
        for (const auto& t : tabs_) {
            auto& tl = labels.emplace_back();
            for (const auto& p : t.plots) {
                auto& pl = tl.emplace_back();
                for (const auto si : p.series)
                    if (si < d.series.size()) pl.push_back(d.series[si].label);
            }
        }
    }

    auto& specs = state_.config().debug_flows;
    while (flow_states_.size() < specs.size()) flow_states_.push_back(std::make_shared<FlowState>());
    flow_states_.resize(specs.size());

    flow_csv_.assign(specs.size(), {});
    std::vector<LogDocument::Series> extra;
    if (doc_.loaded() && !doc_.busy()) {
        for (std::size_t i = 0; i < specs.size(); ++i) {
            auto& st = *flow_states_[i];
            st.log.clear();
            st.log.set_error({});
            auto r = run_offline_flow(specs[i], doc_.data());
            for (const auto& line : r.log) st.log.append(line);
            if (!r.error.empty()) st.log.set_error(r.error);
            else {
                for (auto& s : r.outputs) extra.push_back(std::move(s));
                flow_csv_[i] = std::move(r.tables);
            }
        }
        doc_.set_extra_series(std::move(extra));
    }
    ++xy_epoch_;
    rebuild_tree();

    if (keep_plots) {
        const auto& d = doc_.data();
        std::unordered_map<std::string, std::size_t> by_label;
        for (std::size_t i = 0; i < d.series.size(); ++i) by_label[d.series[i].label] = i;
        for (std::size_t ti = 0; ti < tabs_.size() && ti < labels.size(); ++ti)
            for (std::size_t pi = 0; pi < tabs_[ti].plots.size() && pi < labels[ti].size(); ++pi) {
                auto& series = tabs_[ti].plots[pi].series;
                series.clear();
                for (const auto& l : labels[ti][pi])
                    if (const auto it = by_label.find(l); it != by_label.end()) series.push_back(it->second);
            }
    }
}

void DebugView::add_signal(std::size_t plot, std::size_t series_idx)
{
    auto& tab = tabs_[current_tab_];
    if (plot >= tab.plots.size()) return;
    auto& v = tab.plots[plot].series;
    if (tab.xy && v.size() >= 2) return;   // XY: exactly two signals, "Clear" to change them
    if (std::ranges::find(v, series_idx) == v.end()) {
        // first signal: whole log in X; always fit Y so the new signal is fully visible
        tab.plots[plot].fit_x = tab.plots[plot].fit_x || v.empty();
        tab.plots[plot].fit_y = true;
        v.push_back(series_idx);
    }
    tab.active_plot = plot;
}

void DebugView::view_dbc(const std::string& path)
{
    dbc_error_.clear();
    try {
        dbc_info_ = DbcInfo::load(expand_path(path));
    } catch (const std::exception& e) {
        dbc_info_.reset();
        dbc_error_ = e.what();
    }
    dbc_open_ = true;
}

// ─── layout ─────────────────────────────────────────────────────────────────

void DebugView::render()
{
    // The right mouse button is used for the zoom rectangle (render_plots) instead of ImPlot's own
    // box selection: that one is parked on an unreachable modifier, the plot menu moves to the middle button.
    if (ImPlot::GetCurrentContext()) {
        auto& im = ImPlot::GetInputMap();
        im.Select    = ImGuiMouseButton_Middle;
        im.SelectMod = ImGuiMod_Alt | ImGuiMod_Shift | ImGuiMod_Super;
        im.Menu      = ImGuiMouseButton_Middle;
    }

    if (doc_.poll())
        on_data_changed();

    // keep the history current (not only on save): newest first, one entry per log
    if (const double now = ImGui::GetTime(); doc_.loaded() && !doc_.busy() && now - last_record_ > 3.0) {
        last_record_ = now;
        state_.record_debug_session();
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const bool open = ImGui::Begin("Debug##view", nullptr, flags);
    ImGui::PopStyleVar(2);

    if (open) {
        const float em = ImGui::GetFontSize();
        if (ImGui::BeginTable("##debug_layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("sidebar", ImGuiTableColumnFlags_WidthFixed, em * 26.0f);
            ImGui::TableSetupColumn("plots", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            render_sidebar();
            ImGui::TableNextColumn();
            render_plot_tabs();
            ImGui::EndTable();
        }
        render_dialogs();
    }
    ImGui::End();

    render_dbc_viewer();
    render_unmatched();
    render_spectrum();
}

// ─── sidebar ────────────────────────────────────────────────────────────────

void DebugView::render_sidebar()
{
    render_files();
    ImGui::Separator();

    const float em      = ImGui::GetFontSize();
    const float avail   = ImGui::GetContentRegionAvail().y;
    const float split_h = 6.0f;
    const float flows_h = std::clamp(avail * flows_frac_, em * 6.0f, std::max(em * 6.0f, avail - em * 10.0f));
    render_signal_tree(std::max(em * 6.0f, avail - flows_h - split_h));

    // draggable divider between signals and flows
    ImGui::InvisibleButton("##split", ImVec2(-1, split_h));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive() && avail > 0.0f)
        flows_frac_ = std::clamp(flows_frac_ - ImGui::GetIO().MouseDelta.y / avail, 0.1f, 0.8f);
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(r0.x, (r0.y + r1.y) * 0.5f), ImVec2(r1.x, (r0.y + r1.y) * 0.5f),
                                        ImGui::GetColorU32(ImGuiCol_Separator), 2.0f);

    render_flow_list();
}

// ─── flows ──────────────────────────────────────────────────────────────────

void DebugView::new_flow()
{
    auto& specs = state_.config().debug_flows;
    FlowSpec f;
    f.name = std::format("flow {}", specs.size() + 1);
    specs.push_back(std::move(f));
    apply_flows(true);
    open_flow_tab(specs.size() - 1);
}

void DebugView::delete_flow(std::size_t idx)
{
    auto& specs = state_.config().debug_flows;
    if (idx >= specs.size()) return;
    specs.erase(specs.begin() + static_cast<std::ptrdiff_t>(idx));
    flow_tabs_.clear();   // the editors address flows by index
    apply_flows(true);
}

void DebugView::open_flow_tab(std::size_t idx)
{
    for (const auto& t : flow_tabs_)
        if (t.idx == idx) {
            focus_flow_tab_ = t.id;
            return;
        }
    FlowTab t;
    t.id     = ++flow_tab_counter_;
    t.idx    = idx;
    t.editor = std::make_unique<FlowEditorWindow>(
        state_,
        std::make_unique<OfflineFlowBackend>(state_.config().debug_flows, flow_states_, flow_csv_, doc_, data_version_,
                                             [this] { apply_flows(true); }),
        "Debug Flow");
    t.editor->set_compact(true);
    t.editor->open_flow(idx);
    focus_flow_tab_ = t.id;
    flow_tabs_.push_back(std::move(t));
}

void DebugView::render_flow_list()
{
    if (!ImGui::BeginChild("##flows", ImVec2(0, 0))) {
        ImGui::EndChild();
        return;
    }
    ImGui::TextUnformatted("Flows");
    ImGui::SameLine();
    if (ImGui::SmallButton("+ New"))
        new_flow();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Block diagram over the log signals; its scope outputs appear in the signal list under 'flow'");

    const auto& specs = state_.config().debug_flows;
    if (specs.empty())
        ImGui::TextDisabled("No flows yet");
    std::optional<std::size_t> remove;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const bool err = i < flow_states_.size() && !flow_states_[i]->log.error().empty();
        ImGui::PushID(static_cast<int>(i));
        if (err) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.2f, 0.1f, 1.0f));
        if (ImGui::Selectable(specs[i].name.c_str()))
            open_flow_tab(i);
        if (err) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && err)
            ImGui::SetTooltip("%s", flow_states_[i]->log.error().c_str());
        if (ImGui::BeginPopupContextItem("##flowctx")) {
            if (ImGui::MenuItem("Open")) open_flow_tab(i);
            if (ImGui::MenuItem("Delete")) remove = i;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (remove) delete_flow(*remove);
    ImGui::EndChild();
}

void DebugView::render_files()
{
    const float em = ImGui::GetFontSize();

    ImGui::BeginDisabled(doc_.busy());
    if (ImGui::Button("Open log..."))
        browse_ = Browse::Log;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("History"))
        ImGui::OpenPopup("##history");
    if (ImGui::BeginPopup("##history")) {
        render_history_items();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (doc_.busy()) {
        const std::string text = doc_.status();
        ImGui::ProgressBar(static_cast<float>(doc_.fraction()), ImVec2(-FLT_MIN, 0.0f), text.c_str());
    } else if (doc_.loaded())
        ImGui::TextDisabled("%s  %s", format_name(doc_.data().format), doc_.status().c_str());
    else if (!doc_.status().empty())
        ImGui::TextColored(ImVec4(0.85f, 0.2f, 0.1f, 1.0f), "%s", doc_.status().c_str());
    else
        ImGui::TextDisabled("No log loaded");

    if (!doc_.loaded()) return;
    const auto& d = doc_.data();

    ImGui::TextWrapped("%s", d.path.c_str());

    // Per-interface info; BLF needs a DBC for each one.
    const bool is_blf = d.format == LogDocument::Format::Blf;
    if (ImGui::TreeNodeEx(is_blf ? "Interfaces / DBC" : "Interfaces", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& iface : d.interfaces) {
            ImGui::PushID(iface.c_str());
            ImGui::Text("%s", iface.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%llu frames", static_cast<unsigned long long>(d.frames.at(iface)));
            if (is_blf) {
                ImGui::SetNextItemWidth(-em * 2.5f);
                ImGui::InputTextWithHint("##dbc", "DBC file", &dbcs_[iface]);
                ImGui::SameLine();
                if (ImGui::Button("...")) {
                    browse_       = Browse::Dbc;
                    browse_iface_ = iface;
                }
                ImGui::BeginDisabled(dbcs_[iface].empty());
                if (ImGui::SmallButton("View DBC"))
                    view_dbc(dbcs_[iface]);
                ImGui::EndDisabled();
                if (const auto it = d.dbc_errors.find(iface); it != d.dbc_errors.end())
                    ImGui::TextColored(ImVec4(0.85f, 0.2f, 0.1f, 1.0f), "%s", it->second.c_str());
            }
            ImGui::PopID();
        }
        if (is_blf) {
            ImGui::BeginDisabled(doc_.busy());
            if (ImGui::Button("Apply DBCs"))
                doc_.decode(dbcs_);
            ImGui::EndDisabled();
            ImGui::SameLine();
            const std::string ulabel = "Unmatched (" + std::to_string(d.unmatched.size()) + ")";
            if (!d.unmatched.empty())
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.55f, 0.1f, 1.0f));
            if (ImGui::Button(ulabel.c_str()))
                unmatched_open_ = true;
            if (!d.unmatched.empty())
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("CAN IDs that no DBC message was found for");
            if (d.series.empty() && !doc_.busy())
                ImGui::TextDisabled("BLF holds raw frames only:\nchoose a DBC per interface.");
        }
        ImGui::TreePop();
    }
}

void DebugView::render_signal_tree(float height)
{
    const auto& d = doc_.data();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Filter signals", &filter_);

    if (!ImGui::BeginChild("##signals", ImVec2(0, height - ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_None)) {
        ImGui::EndChild();
        return;
    }
    const bool filtering = !filter_.empty();

    const auto unmatched_label = [](const LogDocument::Unmatched& u) {
        return std::format("0x{:X}{}", u.id, u.id > 0x7FF ? " (ext)" : "");
    };

    for (const auto& [iface, msgs] : tree_) {
        // skip interfaces without a match while filtering
        if (filtering) {
            bool any = false;
            for (const auto& [msg, idxs] : msgs)
                for (const auto i : idxs) any = any || contains_nocase(d.series[i].label, filter_);
            for (const auto& u : d.unmatched)
                any = any || (u.iface == iface && contains_nocase(iface + "/" + unmatched_label(u), filter_));
            if (!any) continue;
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }
        if (!ImGui::TreeNodeEx(iface.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;

        for (const auto& [msg, idxs] : msgs) {
            if (filtering) {
                const bool any = std::ranges::any_of(idxs, [&](auto i) { return contains_nocase(d.series[i].label, filter_); });
                if (!any) continue;
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            }
            if (!ImGui::TreeNode(msg.c_str())) continue;

            // signal names with '/' (flow scopes, e.g. "motor/current/set") form sub-folders
            std::vector<std::size_t> order(idxs);
            if (std::ranges::any_of(order, [&](auto i) { return d.series[i].signal.contains('/'); }))
                std::ranges::stable_sort(order, [&](auto x, auto y) { return d.series[x].signal < d.series[y].signal; });

            struct Level { std::string name; bool open; };
            std::vector<Level> stack;
            const auto close_to = [&](std::size_t n) {
                while (stack.size() > n) {
                    if (stack.back().open) ImGui::TreePop();
                    stack.pop_back();
                }
            };
            for (const auto i : order) {
                const auto& s = d.series[i];
                if (!contains_nocase(s.label, filter_)) continue;

                std::vector<std::string> parts;
                for (std::size_t from = 0;;) {
                    const auto pos = s.signal.find('/', from);
                    parts.push_back(s.signal.substr(from, pos == std::string::npos ? pos : pos - from));
                    if (pos == std::string::npos) break;
                    from = pos + 1;
                }
                const std::string leaf = parts.back();
                parts.pop_back();   // folders

                std::size_t common = 0;
                while (common < stack.size() && common < parts.size() && stack[common].name == parts[common]) ++common;
                close_to(common);
                for (std::size_t k = common; k < parts.size(); ++k) {
                    bool open = stack.empty() || stack.back().open;
                    if (open) {
                        if (filtering) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                        open = ImGui::TreeNode(parts[k].c_str());
                    }
                    stack.push_back({parts[k], open});
                }
                if (!stack.empty() && !stack.back().open) continue;

                ImGui::PushID(static_cast<int>(i));
                ImGui::Selectable(leaf.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    add_signal(tabs_[current_tab_].active_plot, i);
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("CANEO_SIGNAL", &i, sizeof(i));
                    ImGui::TextUnformatted(s.label.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::IsItemHovered() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
                    ImGui::SetTooltip("%s\n%zu samples%s%s\nDouble-click or drag into a plot", s.label.c_str(),
                                      s.t.size(), s.unit.empty() ? "" : ", unit ", s.unit.c_str());
                ImGui::PopID();
            }
            close_to(0);
            ImGui::TreePop();
        }

        // IDs without a DBC message
        std::size_t n_unmatched = 0;
        for (const auto& u : d.unmatched) n_unmatched += u.iface == iface;
        if (n_unmatched > 0) {
            if (filtering) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.55f, 0.1f, 1.0f));
            const bool open = ImGui::TreeNode(std::format("Unmatched ({})###unm", n_unmatched).c_str());
            ImGui::PopStyleColor();
            if (open) {
                for (const auto& u : d.unmatched) {
                    if (u.iface != iface) continue;
                    if (filtering && !contains_nocase(iface + "/" + unmatched_label(u), filter_)) continue;
                    ImGui::TextUnformatted(unmatched_label(u).c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%llu frames, DLC %zu%s", static_cast<unsigned long long>(u.count), u.dlc,
                                        u.has_dbc ? "" : ", no DBC");
                    if (ImGui::IsItemHovered() || ImGui::IsItemClicked()) {
                        std::string hex;
                        for (const auto b : u.last_data) hex += std::format("{:02X} ", b);
                        ImGui::SetTooltip("No DBC message for this ID\nlast data: %s", hex.c_str());
                    }
                }
                ImGui::TreePop();
            }
        }
        ImGui::TreePop();
    }
    ImGui::EndChild();
}

// ─── plots ──────────────────────────────────────────────────────────────────

void DebugView::render_plot_tabs()
{
    if (!ImGui::BeginTabBar("##plot_tabs", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll))
        return;

    std::optional<std::size_t> close;
    bool add = false, add_xy = false;
    if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
        add = true;
    if (ImGui::TabItemButton("New XY plot", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
        add_xy = true;

    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        bool keep = true;
        const bool sel = ImGui::BeginTabItem(tabs_[i].name.c_str(), tabs_.size() > 1 ? &keep : nullptr);
        if (ImGui::BeginPopupContextItem("##tabctx")) {
            if (ImGui::MenuItem("Rename...")) {
                rename_tab_  = i;
                rename_buf_  = tabs_[i].name;
                open_rename_ = true;
            }
            ImGui::EndPopup();
        }
        if (sel) {
            current_tab_ = i;
            render_plot_toolbar();
            if (tabs_[i].xy) render_xy(tabs_[i]);
            else             render_plots(tabs_[i]);
            ImGui::EndTabItem();
        }
        if (!keep) close = i;
        ImGui::PopID();
    }

    std::optional<std::size_t> close_flow;
    for (std::size_t i = 0; i < flow_tabs_.size(); ++i) {
        auto& ft = flow_tabs_[i];
        ImGui::PushID(ft.id);
        bool keep = true;
        const std::string label = "Flow: " + ft.editor->flow_name() + "###flowtab" + std::to_string(ft.id);
        const bool focus = focus_flow_tab_ == ft.id;
        if (ImGui::BeginTabItem(label.c_str(), &keep, focus ? ImGuiTabItemFlags_SetSelected : 0)) {
            ft.editor->render_contents();
            ImGui::EndTabItem();
        }
        if (focus) focus_flow_tab_ = 0;
        if (!keep) close_flow = i;
        ImGui::PopID();
    }
    if (close_flow)
        flow_tabs_.erase(flow_tabs_.begin() + static_cast<std::ptrdiff_t>(*close_flow));
    ImGui::EndTabBar();

    if (open_rename_) {
        open_rename_ = false;
        ImGui::OpenPopup("Rename tab");
    }
    if (ImGui::BeginPopup("Rename tab")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##name", &rename_buf_, ImGuiInputTextFlags_EnterReturnsTrue) && rename_tab_ < tabs_.size()) {
            if (!rename_buf_.empty()) tabs_[rename_tab_].name = rename_buf_;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (close && tabs_.size() > 1) {
        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(*close));
        current_tab_ = std::min(current_tab_, tabs_.size() - 1);
    }
    if (add) {
        tabs_.push_back(PlotTab{"Plot " + std::to_string(++tab_counter_)});
        current_tab_ = tabs_.size() - 1;
    }
    if (add_xy) {
        PlotTab t{"XY " + std::to_string(++tab_counter_)};
        t.xy = true;
        tabs_.push_back(std::move(t));
        current_tab_ = tabs_.size() - 1;
    }
}

void DebugView::render_plot_toolbar()
{
    auto& tab = tabs_[current_tab_];
    ImGui::BeginDisabled(tab.xy);
    if (ImGui::Button("+ Plot")) {
        tab.plots.emplace_back();
        tab.active_plot = tab.plots.size() - 1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(tab.xy || tab.plots.size() <= 1);
    if (ImGui::Button("- Plot")) {
        tab.plots.erase(tab.plots.begin() + static_cast<std::ptrdiff_t>(std::min(tab.active_plot, tab.plots.size() - 1)));
        tab.active_plot = std::min(tab.active_plot, tab.plots.size() - 1);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Remove the selected plot (the one last hovered)");
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        for (auto& p : tab.plots) p.series.clear();
    ImGui::SameLine();
    if (ImGui::Button("Default"))
        default_view(tab);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Default view: equal plot heights, whole log in X, all data in Y (all plots of this tab)  [Space]");
    ImGui::SameLine();
    ImGui::BeginDisabled(tab.undo.empty() && !tab.in_burst);
    if (ImGui::Button("Undo"))
        undo_view(tab);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Undo the last zoom / pan / Default  [Delete / Backspace]");
    ImGui::SameLine();

    // Shortcuts, not while typing into a text field
    if (!ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) default_view(tab);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) undo_view(tab);
    }
    ImGui::BeginDisabled(!doc_.loaded() || doc_.busy());
    if (ImGui::Button("Export CSV"))
        browse_ = Browse::Csv;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Exports every signal of the log (and the flow results) as CSV: time_s,signal,unit,value");
    ImGui::SameLine();
    if (ImGui::Button("Spectrum"))
        spectrum_open_ = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("FFT of the signals of the selected plot (between cursors A / B, or the visible range)");
    ImGui::SameLine();
    if (ImGui::Checkbox("Cursors X", &meas_x_) && meas_x_ && !meas_x_init_) {
        const double w = view_x1_ - view_x0_;
        xa_ = view_x0_ + w / 3.0;
        xb_ = view_x0_ + 2.0 * w / 3.0;
        meas_x_init_ = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Two vertical cursors A / B (drag them): shows dt, 1/dt and the signal values at A and B");
    ImGui::SameLine();
    if (ImGui::Checkbox("Cursors Y", &meas_y_))
        for (auto& p : tab.plots) p.y_init = false;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Two horizontal cursors per plot (drag them): shows dy");
    ImGui::SameLine();
    ImGui::Checkbox("Auto-Y", &auto_y_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep Y fitted to the visible data (turns off when you zoom / pan)");
    ImGui::SameLine();
    ImGui::Checkbox("Stairs", &stairs_);
    ImGui::SameLine();
    ImGui::TextDisabled("Drag signals into a plot; right-drag = zoom rectangle (narrow: Y only, flat: X only); legend right-click = remove.");
}

void DebugView::default_view(PlotTab& tab)
{
    for (auto& p : tab.plots) p.fit_x = p.fit_y = true;
    tab.row_ratios.clear();   // equal heights again
}

static bool views_differ(const DebugView::PlotTab::View& a, const DebugView::PlotTab::View& b)
{
    const auto far = [](double u, double v, double span) { return std::abs(u - v) > 1e-9 * std::max(span, 1e-12); };
    if (far(a.x0, b.x0, a.x1 - a.x0) || far(a.x1, b.x1, a.x1 - a.x0)) return true;
    if (a.has_y && b.has_y && a.y.size() == b.y.size())
        for (std::size_t i = 0; i < a.y.size(); ++i)
            if (far(a.y[i].first, b.y[i].first, a.y[i].second - a.y[i].first) ||
                far(a.y[i].second, b.y[i].second, a.y[i].second - a.y[i].first))
                return true;
    return false;
}

// Called every frame with the limits of all plots; a burst of changes (drag, wheel) becomes one undo step.
void DebugView::record_view(PlotTab& tab, const PlotTab::View& now)
{
    if (now.y.size() != tab.plots.size()) return;   // a plot was not drawn this frame
    const double t = ImGui::GetTime();
    if (tab.skip_frames > 0) {
        --tab.skip_frames;
        tab.last = now;
        tab.last_valid = true;
        tab.in_burst   = false;
        return;
    }
    if (!tab.last_valid || tab.last.y.size() != now.y.size()) {
        tab.last = now;
        tab.last_valid = true;
        tab.undo.clear();
        tab.in_burst = false;
        return;
    }
    if (views_differ(tab.last, now)) {
        if (!tab.in_burst) {
            tab.in_burst    = true;
            tab.burst_start = tab.last;
        }
        tab.last_change = t;
    } else if (tab.in_burst && t - tab.last_change > 0.4) {
        tab.in_burst = false;
        if (views_differ(tab.burst_start, now)) {
            tab.undo.push_back(tab.burst_start);
            if (tab.undo.size() > 100) tab.undo.erase(tab.undo.begin());
        }
    }
    tab.last = now;
}

void DebugView::undo_view(PlotTab& tab)
{
    if (tab.in_burst) {   // close the running burst first
        tab.in_burst = false;
        if (views_differ(tab.burst_start, tab.last)) tab.undo.push_back(tab.burst_start);
    }
    if (tab.undo.empty()) return;
    const PlotTab::View v = tab.undo.back();
    tab.undo.pop_back();
    if (v.y.size() != tab.plots.size()) return;
    for (std::size_t i = 0; i < tab.plots.size(); ++i) {
        tab.plots[i].zoom_x = std::pair{v.x0, v.x1};
        if (v.has_y) tab.plots[i].zoom_y = v.y[i];
    }
    tab.skip_frames = 2;
}

void DebugView::render_plots(PlotTab& tab)
{
    PlotTab::View now;
    now.has_y = !auto_y_;
    const auto& d = doc_.data();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    auto& plots_ = tab.plots;

    bool hovered_any = false;
    const bool fit   = tab.fit_requested;
    tab.fit_requested = false;

    if (tab.row_ratios.size() != plots_.size())
        tab.row_ratios.assign(plots_.size(), 1.0f / static_cast<float>(plots_.size()));

    if (!ImPlot::BeginSubplots("##debug_plots", static_cast<int>(plots_.size()), 1, size,
                               ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_NoTitle |
                                   ImPlotSubplotFlags_ShareItems,
                               tab.row_ratios.data()))
        return;

    std::optional<std::size_t> close_plot;
    for (std::size_t pi = 0; pi < plots_.size(); ++pi) {
        auto& plot = plots_[pi];
        const bool last = pi + 1 == plots_.size();

        const bool full = fit || plot.fit_x;
        if (fit) {
            ImPlot::SetNextAxesToFit();
        } else {
            if (plot.fit_x) ImPlot::SetNextAxisLimits(ImAxis_X1, 0.0, std::max(d.duration, 1e-3), ImPlotCond_Always);
            if (plot.fit_y) ImPlot::SetNextAxisToFit(ImAxis_Y1);
        }
        plot.fit_x = plot.fit_y = false;
        if (plot.zoom_x) {
            ImPlot::SetNextAxisLimits(ImAxis_X1, plot.zoom_x->first, plot.zoom_x->second, ImPlotCond_Always);
            plot.zoom_x.reset();
        }
        if (plot.zoom_y) {
            ImPlot::SetNextAxisLimits(ImAxis_Y1, plot.zoom_y->first, plot.zoom_y->second, ImPlotCond_Always);
            plot.zoom_y.reset();
            auto_y_ = false;   // otherwise the Y axis is locked to the data again
        }
        const std::string plot_id = "##plot" + std::to_string(pi);
        if (!ImPlot::BeginPlot(plot_id.c_str(), ImVec2(-1, -1), ImPlotFlags_NoTitle)) continue;

        // Auto-Y locks the axis to the data, so zooming out is impossible while it is on.
        ImPlot::SetupAxes(last ? "t [s]" : nullptr, nullptr, ImPlotAxisFlags_None,
                          auto_y_ ? (ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit) : ImPlotAxisFlags_None);
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);

        // visible range, used to decimate (whole range on a fit frame)
        const ImPlotRect lim = ImPlot::GetPlotLimits();
        if (pi == 0) { now.x0 = lim.X.Min; now.x1 = lim.X.Max; }
        now.y.emplace_back(lim.Y.Min, lim.Y.Max);
        const double     x0  = full ? 0.0 : lim.X.Min;
        const double     x1  = full ? d.duration : lim.X.Max;
        const int        px  = std::max(64, static_cast<int>(ImPlot::GetPlotSize().x));

        std::optional<std::size_t> remove;
        for (const std::size_t si : plot.series) {
            if (si >= d.series.size()) continue;
            const auto& s = d.series[si];
            if (s.t.empty()) continue;

            const std::string label = s.unit.empty() ? s.label : s.label + " [" + s.unit + "]";

            // [lo, hi) = samples in view, plus one neighbour on each side so lines reach the border
            std::size_t lo = std::lower_bound(s.t.begin(), s.t.end(), x0) - s.t.begin();
            std::size_t hi = std::upper_bound(s.t.begin(), s.t.end(), x1) - s.t.begin();
            if (lo > 0) --lo;
            if (hi < s.t.size()) ++hi;
            const std::size_t n = hi > lo ? hi - lo : 0;
            if (n == 0) continue;

            const double* xs = s.t.data() + lo;
            const double* ys = s.v.data() + lo;
            std::size_t   cnt = n;

            // min/max per pixel column once there are far more samples than pixels
            if (n > static_cast<std::size_t>(px) * 4 && !stairs_) {
                px_.clear();
                py_.clear();
                const std::size_t buckets = static_cast<std::size_t>(px);
                for (std::size_t b = 0; b < buckets; ++b) {
                    const std::size_t a = lo + n * b / buckets;
                    const std::size_t e = lo + n * (b + 1) / buckets;
                    if (a >= e) continue;
                    std::size_t imin = a, imax = a;
                    for (std::size_t i = a; i < e; ++i) {
                        if (s.v[i] < s.v[imin]) imin = i;
                        if (s.v[i] > s.v[imax]) imax = i;
                    }
                    const std::size_t first = std::min(imin, imax), second = std::max(imin, imax);
                    px_.push_back(s.t[first]);  py_.push_back(s.v[first]);
                    if (second != first) { px_.push_back(s.t[second]); py_.push_back(s.v[second]); }
                }
                xs  = px_.data();
                ys  = py_.data();
                cnt = px_.size();
            }

            if (stairs_) ImPlot::PlotStairs(label.c_str(), xs, ys, static_cast<int>(cnt));
            else         ImPlot::PlotLine(label.c_str(), xs, ys, static_cast<int>(cnt));

            if (ImPlot::BeginLegendPopup(label.c_str())) {
                if (ImGui::MenuItem("Remove")) remove = si;
                ImPlot::EndLegendPopup();
            }
        }
        if (remove)
            std::erase(plot.series, *remove);

        if (pi == 0) {
            view_x0_ = lim.X.Min;
            view_x1_ = lim.X.Max;
        }

        // measurement cursors
        if (meas_x_) {
            ImPlot::DragLineX(0, &xa_, ImVec4(0.15f, 0.45f, 0.95f, 1.0f), 1.5f);
            ImPlot::DragLineX(1, &xb_, ImVec4(0.95f, 0.55f, 0.10f, 1.0f), 1.5f);
        }
        if (meas_y_) {
            if (!plot.y_init) {
                plot.ya = lim.Y.Min + (lim.Y.Max - lim.Y.Min) / 3.0;
                plot.yb = lim.Y.Min + 2.0 * (lim.Y.Max - lim.Y.Min) / 3.0;
                plot.y_init = true;
            }
            ImPlot::DragLineY(2, &plot.ya, ImVec4(0.15f, 0.45f, 0.95f, 1.0f), 1.5f);
            ImPlot::DragLineY(3, &plot.yb, ImVec4(0.95f, 0.55f, 0.10f, 1.0f), 1.5f);
        }
        if (meas_x_ || meas_y_) {
            const auto value_at = [](const LogDocument::Series& s, double t) -> std::optional<double> {
                const auto it = std::upper_bound(s.t.begin(), s.t.end(), t);
                if (it == s.t.begin()) return std::nullopt;
                return s.v[static_cast<std::size_t>(it - s.t.begin()) - 1];
            };
            std::string text;
            if (meas_x_) {
                const double dt = xb_ - xa_;
                text += std::format("A = {:.4f} s   B = {:.4f} s\ndt = {:.4f} s", xa_, xb_, dt);
                if (dt != 0.0) text += std::format("   (1/dt = {:.4g} Hz)", 1.0 / std::abs(dt));
                text += '\n';
                for (const std::size_t si : plot.series) {
                    if (si >= d.series.size()) continue;
                    const auto& s  = d.series[si];
                    const auto  va = value_at(s, xa_), vb = value_at(s, xb_);
                    if (!va || !vb) continue;
                    text += std::format("{}: {:.5g} -> {:.5g}   dy = {:.5g} {}\n", s.label, *va, *vb, *vb - *va, s.unit);
                }
            }
            if (meas_y_)
                text += std::format("Ya = {:.5g}   Yb = {:.5g}   dy = {:.5g}\n", plot.ya, plot.yb, plot.yb - plot.ya);

            const ImVec2 pp = ImPlot::GetPlotPos();
            ImDrawList*  dl = ImPlot::GetPlotDrawList();
            const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
            const float  bx = pp.x + 6.0f, by = pp.y + (plots_.size() > 1 ? 30.0f : 6.0f);
            ImPlot::PushPlotClipRect();
            dl->AddRectFilled(ImVec2(bx - 4, by - 2), ImVec2(bx + ts.x + 4, by + ts.y + 2),
                              ImGui::GetColorU32(ImGuiCol_PopupBg, 0.85f), 3.0f);
            dl->AddText(ImVec2(bx, by), ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
            ImPlot::PopPlotClipRect();
        }

        // shared cursor
        if (cursor_valid_) {
            ImPlot::SetNextLineStyle(ImVec4(0.5f, 0.5f, 0.5f, 0.8f), 1.0f);
            ImPlot::PlotInfLines("##cursor", &cursor_x_, 1);
        }

        // close button in the corner (legend sits top right)
        if (plots_.size() > 1) {
            const ImVec2 pp = ImPlot::GetPlotPos();
            ImGui::SetCursorScreenPos(ImVec2(pp.x + 6.0f, pp.y + 6.0f));
            ImGui::SetNextItemAllowOverlap();
            ImGui::PushID(static_cast<int>(pi));
            if (ImGui::SmallButton("x")) close_plot = pi;
            ImGui::PopID();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close this plot");
        }

        // drop target
        if (ImPlot::BeginDragDropTargetPlot()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("CANEO_SIGNAL"))
                add_signal(pi, *static_cast<const std::size_t*>(p->Data));
            ImPlot::EndDragDropTarget();
        }

        // Right-drag zoom rectangle (like VSignalizer): a narrow rectangle zooms Y only, a flat one X only,
        // otherwise both axes.
        {
            const ImGuiIO& io = ImGui::GetIO();
            if (ImPlot::IsPlotHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                const ImPlotPoint p = ImPlot::GetPlotMousePos();
                zoom_plot_  = static_cast<int>(pi);
                zoom_start_ = io.MousePos;
                zoom_x0_    = p.x;
                zoom_y0_    = p.y;
            }
            if (zoom_plot_ == static_cast<int>(pi)) {
                constexpr float SMALL = 14.0f, MIN_DRAG = 4.0f;   // px
                if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    zoom_plot_ = -1;
                } else {
                    const float dx = std::abs(io.MousePos.x - zoom_start_.x), dy = std::abs(io.MousePos.y - zoom_start_.y);
                    const bool  narrow = dx < SMALL, flat = dy < SMALL;
                    const bool  y_only = narrow && !flat && dy >= MIN_DRAG, x_only = flat && !narrow && dx >= MIN_DRAG;

                    const ImVec2 pp = ImPlot::GetPlotPos(), ps = ImPlot::GetPlotSize();
                    ImVec2       a = zoom_start_, b = io.MousePos;
                    if (y_only) { a.x = pp.x; b.x = pp.x + ps.x; }
                    if (x_only) { a.y = pp.y; b.y = pp.y + ps.y; }
                    if (y_only || x_only || (!narrow && !flat)) {
                        ImDrawList* dl = ImPlot::GetPlotDrawList();
                        ImPlot::PushPlotClipRect();
                        dl->AddRectFilled(a, b, IM_COL32(60, 130, 230, 50));
                        dl->AddRect(a, b, IM_COL32(60, 130, 230, 220));
                        ImPlot::PopPlotClipRect();
                    }

                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                        zoom_plot_ = -1;
                        const ImPlotPoint p = ImPlot::GetPlotMousePos();
                        const auto xr = std::minmax(zoom_x0_, p.x), yr = std::minmax(zoom_y0_, p.y);
                        if (y_only || (!narrow && !flat)) plot.zoom_y = std::pair{yr.first, yr.second};
                        if (x_only || (!narrow && !flat)) plot.zoom_x = std::pair{xr.first, xr.second};
                    }
                }
            }
        }

        // any manual zoom / pan hands the Y axis back to the user
        if (auto_y_ && ((ImPlot::IsPlotHovered() && ImGui::GetIO().MouseWheel != 0.0f) ||
                        (ImPlot::IsAxisHovered(ImAxis_Y1) && ImGui::IsMouseDragging(ImGuiMouseButton_Left))))
            auto_y_ = false;

        if (ImPlot::IsPlotHovered()) {
            hovered_any  = true;
            tab.active_plot = pi;
            cursor_x_    = ImPlot::GetPlotMousePos().x;

            // values at the cursor (sample-and-hold)
            if (!plot.series.empty() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && zoom_plot_ < 0) {
                ImGui::BeginTooltip();
                ImGui::Text("t = %.4f s", cursor_x_);
                for (const std::size_t si : plot.series) {
                    if (si >= d.series.size()) continue;
                    const auto& s  = d.series[si];
                    const auto  it = std::upper_bound(s.t.begin(), s.t.end(), cursor_x_);
                    if (it == s.t.begin()) continue;
                    const double v = s.v[static_cast<std::size_t>(it - s.t.begin()) - 1];
                    ImGui::Text("%s = %g %s", s.label.c_str(), v, s.unit.c_str());
                }
                ImGui::EndTooltip();
            }
        }
        ImPlot::EndPlot();
    }
    ImPlot::EndSubplots();
    record_view(tab, now);
    cursor_valid_ = hovered_any;
    if (close_plot && plots_.size() > 1) {
        plots_.erase(plots_.begin() + static_cast<std::ptrdiff_t>(*close_plot));
        tab.active_plot = std::min(tab.active_plot, plots_.size() - 1);
    }
}

// XY plot: the first signal is X, the second Y. Y is interpolated onto the sample times of X.
void DebugView::render_xy(PlotTab& tab)
{
    const auto& d = doc_.data();
    auto&       plot = tab.plots.front();
    tab.active_plot  = 0;

    const bool two = plot.series.size() == 2 && plot.series[0] < d.series.size() && plot.series[1] < d.series.size();
    ImGui::Checkbox("Equal axes", &tab.xy_equal);
    ImGui::SameLine();
    if (two)
        ImGui::TextDisabled("X = %s    Y = %s", d.series[plot.series[0]].label.c_str(), d.series[plot.series[1]].label.c_str());
    else
        ImGui::TextDisabled("Drag two signals into the plot: the first is X, the second is Y (%zu/2).", plot.series.size());

    if (two) {
        const auto& sx = d.series[plot.series[0]];
        const auto& sy = d.series[plot.series[1]];
        const std::vector<std::size_t> key{plot.series[0], plot.series[1], sx.t.size(), sy.t.size(), xy_epoch_};
        if (tab.xy_key != key) {
            tab.xy_key = key;
            tab.xy_x.clear();
            tab.xy_y.clear();
            for (std::size_t i = 0; i < sx.t.size(); ++i) {
                const double t = sx.t[i];
                if (sy.t.empty() || t < sy.t.front() || t > sy.t.back()) continue;
                const auto it = std::lower_bound(sy.t.begin(), sy.t.end(), t);
                const std::size_t j = static_cast<std::size_t>(it - sy.t.begin());
                double y = sy.v[j];
                if (j > 0 && sy.t[j] > t) {
                    const double t0 = sy.t[j - 1], t1 = sy.t[j];
                    y = sy.v[j - 1] + (sy.v[j] - sy.v[j - 1]) * (t - t0) / (t1 - t0);
                }
                tab.xy_x.push_back(sx.v[i]);
                tab.xy_y.push_back(y);
            }
            plot.fit_x = plot.fit_y = true;
        }
    } else if (!tab.xy_key.empty()) {
        tab.xy_key.clear();
        tab.xy_x.clear();
        tab.xy_y.clear();
    }

    if (tab.fit_requested || plot.fit_x || plot.fit_y) {
        ImPlot::SetNextAxesToFit();
        tab.fit_requested = plot.fit_x = plot.fit_y = false;
    }
    if (plot.zoom_x) { ImPlot::SetNextAxisLimits(ImAxis_X1, plot.zoom_x->first, plot.zoom_x->second, ImPlotCond_Always); plot.zoom_x.reset(); }
    if (plot.zoom_y) { ImPlot::SetNextAxisLimits(ImAxis_Y1, plot.zoom_y->first, plot.zoom_y->second, ImPlotCond_Always); plot.zoom_y.reset(); }

    if (!ImPlot::BeginPlot("##xyplot", ImVec2(-1, -1), ImPlotFlags_NoTitle | (tab.xy_equal ? ImPlotFlags_Equal : 0)))
        return;
    std::string xl = "x", yl = "y";
    if (two) {
        const auto& sx = d.series[plot.series[0]];
        const auto& sy = d.series[plot.series[1]];
        xl = sx.unit.empty() ? sx.label : sx.label + " [" + sx.unit + "]";
        yl = sy.unit.empty() ? sy.label : sy.label + " [" + sy.unit + "]";
    }
    ImPlot::SetupAxes(xl.c_str(), yl.c_str());
    ImPlot::SetupLegend(ImPlotLocation_NorthEast);

    if (two && !tab.xy_x.empty()) {
        // very long logs: thin out evenly
        const std::size_t n = tab.xy_x.size(), max_pts = 200000;
        const int stride = static_cast<int>(std::max<std::size_t>(1, (n + max_pts - 1) / max_pts));
        ImPlot::PlotLine("path", tab.xy_x.data(), tab.xy_y.data(), static_cast<int>(n / static_cast<std::size_t>(stride)), 0, 0,
                         static_cast<int>(sizeof(double)) * stride);
        // start / end
        const double sxp[2] = {tab.xy_x.front(), tab.xy_x.back()}, syp[2] = {tab.xy_y.front(), tab.xy_y.back()};
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 6);
        ImPlot::PlotScatter("start", &sxp[0], &syp[0], 1);
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Square, 6);
        ImPlot::PlotScatter("end", &sxp[1], &syp[1], 1);
    }

    if (ImPlot::BeginDragDropTargetPlot()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("CANEO_SIGNAL"))
            add_signal(0, *static_cast<const std::size_t*>(p->Data));
        ImPlot::EndDragDropTarget();
    }
    ImPlot::EndPlot();
}

// ─── spectrum ───────────────────────────────────────────────────────────────

void DebugView::render_spectrum()
{
    if (!spectrum_open_) return;
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 50, em * 28), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Spectrum###spectrum", &spectrum_open_) && doc_.loaded()) {
        const auto& d    = doc_.data();
        const auto& tab  = tabs_[std::min(current_tab_, tabs_.size() - 1)];
        const std::size_t pi = std::min(tab.active_plot, tab.plots.size() - 1);
        const auto& series = tab.plots[pi].series;

        double ta = std::max(0.0, view_x0_), tb = std::min(d.duration, view_x1_);
        if (meas_x_) {
            ta = std::min(xa_, xb_);
            tb = std::max(xa_, xb_);
        }
        if (ImGui::Button("Default")) spec_fit_ = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fit both axes to the spectrum");
        ImGui::SameLine();
        if (ImGui::Checkbox("log f", &spec_log_x_)) spec_fit_ = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("dB", &spec_db_)) spec_fit_ = true;
        ImGui::SameLine();
        ImGui::TextDisabled("Plot %zu, %s %.3f .. %.3f s", pi + 1, meas_x_ ? "cursors A-B:" : "visible range:", ta, tb);

        if (spec_key_series_ != series || spec_key_a_ != ta || spec_key_b_ != tb || spec_key_version_ != data_version_) {
            spec_key_series_  = series;
            spec_key_a_       = ta;
            spec_key_b_       = tb;
            spec_key_version_ = data_version_;
            spec_fit_         = true;
            spec_.clear();
            for (const std::size_t si : series) {
                if (si >= d.series.size()) continue;
                const auto& s = d.series[si];
                Spec        r;
                auto        res = spectrum_of(s, ta, tb, r.fs, r.samples, r.nfft);
                r.label = s.label;
                r.unit  = s.unit;
                if (res) {
                    r.f = std::move(res->first);
                    r.a = std::move(res->second);
                    for (std::size_t k = 1; k < r.a.size(); ++k)   // skip DC
                        if (r.a[k] > r.peak_a) { r.peak_a = r.a[k]; r.peak_f = r.f[k]; }
                }
                spec_.push_back(std::move(r));
            }
        }

        if (series.empty())
            ImGui::TextDisabled("The selected plot has no signals.");
        const float table_h = ImGui::GetTextLineHeightWithSpacing() * (static_cast<float>(spec_.size()) + 1.5f);
        if (spec_fit_) ImPlot::SetNextAxesToFit();
        spec_fit_ = false;
        if (ImPlot::BeginPlot("##spectrum", ImVec2(-1, std::max(em * 8, ImGui::GetContentRegionAvail().y - table_h)),
                              ImPlotFlags_NoTitle)) {
            ImPlot::SetupAxes("f [Hz]", spec_db_ ? "amplitude [dB]" : "amplitude");
            if (spec_log_x_) ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
            ImPlot::SetupLegend(ImPlotLocation_NorthEast);
            const int px = std::max(64, static_cast<int>(ImPlot::GetPlotSize().x));
            for (const auto& r : spec_) {
                if (r.f.empty()) continue;
                const std::size_t start = spec_log_x_ ? 1 : 0;   // log axis cannot show f = 0
                const std::size_t n     = r.f.size() - start;
                spec_px_.clear();
                spec_py_.clear();
                const auto val = [&](std::size_t k) { return spec_db_ ? 20.0 * std::log10(std::max(r.a[k], 1e-12)) : r.a[k]; };
                if (n > static_cast<std::size_t>(px) * 4) {   // max per pixel column
                    const auto buckets = static_cast<std::size_t>(px);
                    for (std::size_t b = 0; b < buckets; ++b) {
                        const std::size_t a0 = start + n * b / buckets, a1 = start + n * (b + 1) / buckets;
                        if (a0 >= a1) continue;
                        std::size_t im = a0;
                        for (std::size_t k = a0; k < a1; ++k)
                            if (r.a[k] > r.a[im]) im = k;
                        spec_px_.push_back(r.f[im]);
                        spec_py_.push_back(val(im));
                    }
                } else {
                    for (std::size_t k = start; k < r.f.size(); ++k) {
                        spec_px_.push_back(r.f[k]);
                        spec_py_.push_back(val(k));
                    }
                }
                ImPlot::PlotLine(r.label.c_str(), spec_px_.data(), spec_py_.data(), static_cast<int>(spec_px_.size()));
            }
            ImPlot::EndPlot();
        }
        for (const auto& r : spec_) {
            if (r.f.empty())
                ImGui::TextDisabled("%s: too few samples in the range", r.label.c_str());
            else
                ImGui::Text("%s: peak %.4g Hz, amplitude %.4g %s   (fs %.4g Hz, %zu samples, df %.4g Hz)", r.label.c_str(),
                            r.peak_f, r.peak_a, r.unit.c_str(), r.fs, r.samples, r.fs / static_cast<double>(r.nfft));
        }
    }
    ImGui::End();
}

// ─── file dialogs ───────────────────────────────────────────────────────────

void DebugView::render_dialogs()
{
    if (browse_ != Browse::None) {
        if (browse_ == Browse::Log) {
            std::filesystem::path start = log_path_.empty() ? state_.config().log_dir().value_or(std::filesystem::path{})
                                                            : expand_path(log_path_);
            browser_.open("Open log", FileBrowser::Mode::Open, {".mcap", ".blf"}, start);
        } else if (browse_ == Browse::Csv) {
            auto start = expand_path(log_path_);
            browser_.open("Export all signals", FileBrowser::Mode::Save, {".csv"}, start.replace_extension(".csv"));
        } else {
            browser_.open("Select DBC", FileBrowser::Mode::Open, {".dbc"}, expand_path(dbcs_[browse_iface_]));
        }
        // handled below via pending_
        pending_ = browse_;
        browse_  = Browse::None;
    }

    if (const auto chosen = browser_.render()) {
        if (pending_ == Browse::Log) {
            load(chosen->string());
        } else if (pending_ == Browse::Csv) {
            std::size_t rows = 0;
            auto        path = *chosen;
            if (path.extension().empty()) path.replace_extension(".csv");
            const auto err = write_series_csv(path, doc_.data().series, rows);
            state_.set_status(err.empty() ? std::format("Exported {} samples to {}", rows, path.string()) : err, !err.empty());
        } else if (pending_ == Browse::Dbc) {
            dbcs_[browse_iface_] = chosen->string();
            if (doc_.loaded() && doc_.data().format == LogDocument::Format::Blf)
                doc_.decode(dbcs_);
        }
    }
}

// ─── DBC viewer / unmatched ─────────────────────────────────────────────────

void DebugView::render_dbc_viewer()
{
    if (!dbc_open_) return;
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 60, em * 36), ImGuiCond_FirstUseEver);
    const std::string title = "DBC" + (dbc_info_ ? ": " + std::filesystem::path(dbc_info_->path).filename().string() : "") +
                              "###dbcviewer";
    if (ImGui::Begin(title.c_str(), &dbc_open_)) {
        if (!dbc_info_) {
            ImGui::TextColored(ImVec4(0.85f, 0.2f, 0.1f, 1.0f), "%s", dbc_error_.c_str());
        } else {
            std::size_t signals = 0;
            for (const auto& m : dbc_info_->messages) signals += m.signals.size();
            ImGui::TextDisabled("%s - %zu messages, %zu signals", dbc_info_->path.c_str(), dbc_info_->messages.size(), signals);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##dbcfilter", "Filter: message, signal, ID (0x123), sender, unit, comment", &dbc_filter_);

            if (ImGui::BeginChild("##dbctree")) {
                for (const auto& m : dbc_info_->messages) {
                    const std::string idstr = std::format("0x{:X}{}", m.id, m.extended ? " (ext)" : "");
                    // message matches -> all its signals; otherwise only the matching signals
                    const bool msg_hit = dbc_filter_.empty() || contains_nocase(m.name, dbc_filter_) ||
                                         contains_nocase(idstr, dbc_filter_) || contains_nocase(m.sender, dbc_filter_);
                    const auto sig_hit = [&](const DbcInfo::Signal& s) {
                        return msg_hit || contains_nocase(s.name, dbc_filter_) || contains_nocase(s.unit, dbc_filter_) ||
                               contains_nocase(s.comment, dbc_filter_);
                    };
                    if (!dbc_filter_.empty()) {
                        if (!std::ranges::any_of(m.signals, sig_hit)) continue;
                        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                    }
                    const std::string head = std::format("{}  {}  [{} B, {} signals]###m{}", idstr, m.name, m.size,
                                                         m.signals.size(), m.id | (m.extended ? 0x80000000u : 0u));
                    if (!ImGui::TreeNode(head.c_str())) continue;
                    if (!m.sender.empty()) ImGui::TextDisabled("Sender: %s", m.sender.c_str());
                    if (!m.comment.empty()) ImGui::TextWrapped("%s", m.comment.c_str());

                    if (ImGui::BeginTable("##sigs", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                        for (const char* h : {"Signal", "Start|Len", "Order", "Type", "Factor|Offset", "Min|Max", "Unit", "Receivers"})
                            ImGui::TableSetupColumn(h);
                        ImGui::TableHeadersRow();
                        for (const auto& s : m.signals) {
                            if (!sig_hit(s)) continue;
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%s%s%s", s.name.c_str(), s.mux.empty() ? "" : " ", s.mux.c_str());
                            if (ImGui::IsItemHovered() && (!s.comment.empty() || !s.values.empty())) {
                                ImGui::BeginTooltip();
                                if (!s.comment.empty()) ImGui::TextUnformatted(s.comment.c_str());
                                for (const auto& v : s.values) ImGui::TextUnformatted(v.c_str());
                                ImGui::EndTooltip();
                            }
                            ImGui::TableNextColumn(); ImGui::Text("%llu|%llu", (unsigned long long)s.start_bit, (unsigned long long)s.bit_size);
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.little_endian ? "Intel" : "Motorola");
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.is_signed ? "signed" : "unsigned");
                            ImGui::TableNextColumn(); ImGui::Text("%g|%g", s.factor, s.offset);
                            ImGui::TableNextColumn(); ImGui::Text("%g|%g", s.min, s.max);
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.unit.c_str());
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.receivers.c_str());
                        }
                        ImGui::EndTable();
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
}

void DebugView::render_unmatched()
{
    if (!unmatched_open_) return;
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 55, em * 24), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Unmatched messages###unmatched", &unmatched_open_)) {
        const auto& u = doc_.data().unmatched;
        if (u.empty()) {
            ImGui::TextUnformatted("Every CAN ID was assigned to a DBC message.");
        } else {
            ImGui::TextWrapped("CAN IDs for which no DBC message exists (or the payload is shorter than the DBC message). "
                               "They are not plotted.");
            if (ImGui::BeginTable("##unmatched", 7,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Interface");
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_DefaultSort);
                ImGui::TableSetupColumn("Frames");
                ImGui::TableSetupColumn("DLC");
                ImGui::TableSetupColumn("First [s]");
                ImGui::TableSetupColumn("Last data");
                ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_NoSort);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                std::vector<const LogDocument::Unmatched*> rows;
                for (const auto& r : u) rows.push_back(&r);
                if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs(); sp && sp->SpecsCount > 0) {
                    const bool asc = sp->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                    const int  col = sp->Specs[0].ColumnIndex;
                    std::ranges::stable_sort(rows, [&](const auto* a, const auto* b) {
                        auto key = [&](const auto* r) -> double {
                            switch (col) {
                                case 2: return static_cast<double>(r->count);
                                case 3: return static_cast<double>(r->dlc);
                                case 4: return r->first_t;
                                default: return r->id;
                            }
                        };
                        if (col == 0) return asc ? a->iface < b->iface : a->iface > b->iface;
                        return asc ? key(a) < key(b) : key(a) > key(b);
                    });
                }
                for (const auto* r : rows) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(r->iface.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("0x%X%s", r->id, r->id > 0x7FF ? " (ext)" : "");
                    ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(r->count));
                    ImGui::TableNextColumn(); ImGui::Text("%zu", r->dlc);
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", r->first_t);
                    ImGui::TableNextColumn();
                    std::string hex;
                    for (const auto b : r->last_data) hex += std::format("{:02X} ", b);
                    ImGui::TextUnformatted(hex.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r->has_dbc ? "ID not in DBC" : "no DBC for interface");
                }
                ImGui::EndTable();
            }
        }
    }
    ImGui::End();
}
