#include "XcpView.hpp"

#include "AppState.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <limits>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>

namespace {

constexpr std::size_t MAX_POINTS  = 400'000;   // per series; the oldest 100k are dropped beyond that
constexpr std::size_t MAX_FILTER  = 5'000;     // rows shown for a filter
const char*           DRAG_TYPE   = "CANEO_XCP_VAR";

const ImVec4 COL_ERROR = ImVec4(0.85f, 0.20f, 0.10f, 1.0f);
const ImVec4 COL_OK    = ImVec4(0.15f, 0.60f, 0.20f, 1.0f);
const ImVec4 COL_WARN  = ImVec4(0.80f, 0.50f, 0.00f, 1.0f);

bool interface_up(const std::string& name)
{
    std::ifstream f("/sys/class/net/" + name + "/operstate");
    std::string   s;
    return (f >> s) && (s == "up" || s == "unknown");
}

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "12.5", "-3", "0x1F", "true" / "false", or the name of an enumerator.
std::optional<double> parse_value(std::string text, const ElfVariable* var)
{
    std::erase_if(text, [](unsigned char c) { return std::isspace(c); });
    if (text.empty()) return std::nullopt;
    if (var)
        for (const auto& [v, n] : var->enumerators)
            if (n == text) return static_cast<double>(v);
    const std::string l = lower(text);
    if (l == "true" || l == "on") return 1.0;
    if (l == "false" || l == "off") return 0.0;
    if (l.starts_with("0x") || l.starts_with("-0x")) {
        const bool neg = l.front() == '-';
        uint64_t   u   = 0;
        const char* b  = l.data() + (neg ? 3 : 2);
        const auto [p, ec] = std::from_chars(b, l.data() + l.size(), u, 16);
        if (ec != std::errc{} || p != l.data() + l.size()) return std::nullopt;
        return neg ? -static_cast<double>(u) : static_cast<double>(u);
    }
    char*        end = nullptr;
    const double d   = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size()) return std::nullopt;
    return d;
}

}   // namespace

XcpView::XcpView(AppState& state)
    : state_(state)
{
    state_.set_xcp_snapshot([this] { return snapshot(); });
}

XcpView::~XcpView()
{
    state_.set_xcp_snapshot({});
    master_.reset();   // joins the worker before data_ goes away
}

std::ostream& operator<<(std::ostream& os, const XcpView& v)
{
    return os << "XcpView(" << (v.elf_ ? v.elf_->path : std::string("no ELF")) << (v.a2l_path_.empty() ? "" : " + " + v.a2l_path_) << ", " << v.measure_.size()
              << " measured, " << v.plots_.size() << " plot(s), " << (v.master_ ? to_string(v.master_->state()) : "no master")
              << ")";
}

XcpConfig XcpView::snapshot() const
{
    XcpConfig c;
    c.target  = target_name_;
    c.elf     = elf_path_;
    c.a2l     = a2l_path_;
    c.poll_ms = poll_ms_;
    c.measure = measure_;
    for (const auto& p : plots_) c.plots.push_back(p.names);
    if (c.plots.size() == 1 && c.plots.front().empty()) c.plots.clear();
    return c;
}

void XcpView::apply_config()
{
    applied_     = true;
    config_path_ = state_.path();
    const auto& c = state_.config().xcp;
    target_name_ = c.target;
    poll_ms_     = c.poll_ms;
    measure_     = c.measure;
    plots_.clear();
    for (const auto& names : c.plots) plots_.push_back(Plot{names});
    if (plots_.empty()) plots_.emplace_back();
    active_plot_ = 0;
    const bool elf = !c.elf.empty() && c.elf != elf_path_;
    const bool a2l = !c.a2l.empty() && c.a2l != a2l_path_;
    if (elf) load_elf(c.elf);
    if (a2l) load_a2l(c.a2l);
    if (!elf && !a2l) push_measurements();
}

const XcpTargetConfig* XcpView::target() const
{
    const auto& targets = state_.config().xcp_targets;
    for (const auto& t : targets)
        if (t.name == target_name_) return &t;
    return targets.empty() ? nullptr : &targets.front();
}

// ── ELF ──────────────────────────────────────────────────────────────────────

void XcpView::load_elf(const std::string& path)
{
    elf_path_  = path;
    elf_error_.clear();
    const std::string full = expand_path(path).string();
    elf_loading_ = std::async(std::launch::async, [full] { return std::make_unique<ElfImage>(::load_elf(full)); });
}

void XcpView::load_a2l(const std::string& path)
{
    a2l_path_ = path;
    a2l_error_.clear();
    const std::string full = expand_path(path).string();
    a2l_loading_ = std::async(std::launch::async, [full] { return std::make_unique<A2lModule>(::load_a2l(full)); });
}

void XcpView::close_a2l()
{
    a2l_.reset();
    a2l_path_.clear();
    a2l_error_.clear();
    rebuild_image();
}

void XcpView::poll_elf()
{
    const auto ready = [](const auto& f) {
        return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    };
    bool changed = false;
    if (ready(elf_loading_)) {
        try {
            elf_ = elf_loading_.get();
            log_.push_back(std::format("ELF loaded: {} variables, {} scalars{}", elf_->variables.size(), elf_->leaf_count(),
                                       elf_->has_dwarf ? "" : " (no debug info)"));
            for (const auto& w : elf_->warnings) log_.push_back("ELF: " + w);
            changed = true;
        } catch (const std::exception& e) {
            elf_error_ = e.what();
            log_.push_back("ELF: " + elf_error_);
        }
    }
    if (ready(a2l_loading_)) {
        try {
            a2l_ = a2l_loading_.get();
            std::size_t chars = 0;
            for (const auto& o : a2l_->objects) chars += o.characteristic;
            log_.push_back(std::format("A2L loaded: {} measurements, {} characteristics", a2l_->objects.size() - chars, chars));
            changed = true;
        } catch (const std::exception& e) {
            a2l_error_ = e.what();
            log_.push_back("A2L: " + a2l_error_);
        }
    }
    if (changed) rebuild_image();
}

void XcpView::rebuild_image()
{
    image_.reset();
    if (a2l_) {
        // wait for an ELF that is still loading: its addresses replace the A2L ones
        if (elf_loading_.valid()) return;
        image_ = std::make_unique<ElfImage>(a2l_image(*a2l_, elf_.get()));
        for (const auto& w : image_->warnings) log_.push_back("A2L: " + w);
    }
    applied_filter_ = "\x01";   // rebuild the filter list
    if (const auto* img = vars()) {
        std::size_t missing = 0;
        for (const auto& m : measure_)
            if (!img->find(m.name)) ++missing;
        if (missing) log_.push_back(std::format("{} measured variable(s) are not in the {}", missing, image_ ? "A2L" : "ELF"));
    }
    push_measurements();
}

std::string XcpView::display_name(const std::string& name) const
{
    const auto* v = find(name);
    return v && !v->description.empty() ? v->description : name;
}

// ── measuring ────────────────────────────────────────────────────────────────

void XcpView::connect()
{
    const auto* t = target();
    if (!t) return;
    if (!interface_up(t->interface)) {
        log_.push_back(std::format("{} is down - press Start in the menu bar first", t->interface));
        return;
    }
    target_name_ = t->name;
    master_.reset();
    {
        std::lock_guard lock(data_mutex_);
        data_.clear();
        latest_t_ = 0.0;
    }
    master_ = std::make_unique<XcpMaster>(*t);
    master_->set_sample_fn([this](const std::string& n, double time, double v) { on_sample(n, time, v); });
    master_->connect();
    push_measurements();
    live_ = true;
}

void XcpView::on_sample(const std::string& name, double t, double v)
{
    {
        std::lock_guard lock(data_mutex_);
        auto& s = data_[name];
        if (!s.t.empty() && t < s.t.back()) t = s.t.back();   // keep the series sorted (poll vs. DAQ jitter)
        s.t.push_back(t);
        s.v.push_back(v);
        s.last   = v;
        s.last_t = t;
        s.has    = true;
        if (s.t.size() > MAX_POINTS) {
            s.t.erase(s.t.begin(), s.t.begin() + 100'000);
            s.v.erase(s.v.begin(), s.v.begin() + 100'000);
        }
        latest_t_ = std::max(latest_t_, t);
    }
    state_.log_scalar("xcp/" + name, v);
}

void XcpView::push_measurements()
{
    if (!master_ || !vars()) return;
    const auto* t = target();
    const uint8_t ext = t ? static_cast<uint8_t>(t->addr_ext) : 0;
    std::vector<XcpVar> vars;
    for (const auto& m : measure_)
        if (const auto* v = find(m.name)) vars.push_back(XcpVar::from(*v, ext, m.event));
    master_->set_measurements(std::move(vars), poll_ms_);
}

bool XcpView::measured(const std::string& name) const
{
    return std::ranges::any_of(measure_, [&](const auto& m) { return m.name == name; });
}

void XcpView::measure(const std::string& name, int event)
{
    auto it = std::ranges::find(measure_, name, &XcpMeasureConfig::name);
    if (it == measure_.end()) measure_.push_back({name, event});
    else it->event = event;
    push_measurements();
}

void XcpView::unmeasure(const std::string& name)
{
    std::erase_if(measure_, [&](const auto& m) { return m.name == name; });
    for (auto& p : plots_) std::erase(p.names, name);
    push_measurements();
}

void XcpView::add_to_plot(const std::string& name, std::size_t plot)
{
    if (plot >= plots_.size()) {
        plots_.emplace_back();
        plot = plots_.size() - 1;
    }
    auto& names = plots_[plot].names;
    if (!std::ranges::contains(names, name)) names.push_back(name);
    active_plot_ = plot;
    if (!measured(name)) measure(name, -1);
}

void XcpView::open_edit(const std::string& name)
{
    edit_name_ = name;
    edit_error_.clear();
    edit_text_.clear();
    {
        std::lock_guard lock(data_mutex_);
        if (const auto it = data_.find(name); it != data_.end() && it->second.has)
            edit_text_ = std::format("{}", it->second.last);
    }
    open_edit_ = true;
    if (master_ && master_->state() == XcpMaster::State::Connected)
        if (const auto* v = find(name)) {
            const auto* t = target();
            master_->read(XcpVar::from(*v, t ? static_cast<uint8_t>(t->addr_ext) : 0));
        }
}

std::string XcpView::format_value(const ElfVariable* var, double v) const
{
    if (var) {
        for (const auto& [e, n] : var->enumerators)
            if (static_cast<double>(e) == v) return std::format("{} ({})", n, e);
        if (var->type == XcpType::Bool) return v != 0.0 ? "true" : "false";
        if (xcp_type_float(var->type)) return std::format("{:.6g}", v);
    }
    if (std::abs(v) < 1e15 && v == std::floor(v)) return std::format("{}", static_cast<int64_t>(v));
    return std::format("{:.6g}", v);
}

std::string XcpView::event_label(int event) const
{
    if (event < 0) return std::format("poll {} ms", poll_ms_);
    if (master_)
        for (const auto& e : master_->events())
            if (e.number == event) {
                if (e.cycle_s > 0) return std::format("DAQ {} ({:g} ms)", e.name, e.cycle_s * 1e3);
                return "DAQ " + e.name;
            }
    return std::format("DAQ event {}", event);
}

// ── rendering ────────────────────────────────────────────────────────────────

void XcpView::render_menu_items()
{
    if (ImGui::MenuItem("Load ELF...")) open_elf_dialog();
    if (ImGui::MenuItem("Reload ELF", nullptr, false, !elf_path_.empty())) load_elf(elf_path_);
    ImGui::Separator();
    if (ImGui::MenuItem("Load A2L...")) open_a2l_dialog();
    if (ImGui::MenuItem("Reload A2L", nullptr, false, !a2l_path_.empty())) load_a2l(a2l_path_);
    if (ImGui::MenuItem("Close A2L (variables from the ELF)", nullptr, false, !a2l_path_.empty())) close_a2l();
    ImGui::Separator();
    const bool connected = master_ && master_->state() != XcpMaster::State::Disconnected;
    if (ImGui::MenuItem(connected ? "Disconnect" : "Connect", nullptr, false, target() != nullptr)) {
        if (connected) master_->disconnect();
        else connect();
    }
}

void XcpView::render()
{
    if (!applied_ || config_path_ != state_.path()) apply_config();
    poll_elf();
    if (master_)
        for (auto& m : master_->take_messages()) log_.push_back(std::move(m));
    while (log_.size() > 300) log_.pop_front();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const bool open = ImGui::Begin("XCP##view", nullptr, flags);
    ImGui::PopStyleVar(2);
    if (open) {
        render_toolbar();
        ImGui::Separator();
        const float em = ImGui::GetFontSize();
        if (ImGui::BeginTable("##xcp_layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("plots", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("variables", ImGuiTableColumnFlags_WidthFixed, em * 30.0f);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            render_plots();
            ImGui::TableNextColumn();
            render_sidebar();
            ImGui::EndTable();
        }
        render_edit_popup();

        if (open_browser_ == Browse::Elf) {
            // executables without extension (e.g. _BUILD/linux/model) are recognised by the ELF magic
            browser_.open("Load ELF", FileBrowser::Mode::Open, {".elf", ".axf", ".out"},
                          elf_path_.empty() ? std::filesystem::path{} : expand_path(elf_path_), FileBrowser::is_elf,
                          "ELF executables");
        } else if (open_browser_ == Browse::A2l) {
            const std::string start = !a2l_path_.empty() ? a2l_path_ : elf_path_;
            browser_.open("Load A2L", FileBrowser::Mode::Open, {".a2l"},
                          start.empty() ? std::filesystem::path{} : expand_path(start).parent_path());
        }
        if (open_browser_ != Browse::None) browsing_ = std::exchange(open_browser_, Browse::None);
        if (const auto chosen = browser_.render()) {
            if (browsing_ == Browse::A2l) load_a2l(chosen->string());
            else load_elf(chosen->string());
        }
    }
    ImGui::End();
}

void XcpView::render_toolbar()
{
    const float em = ImGui::GetFontSize();
    const auto& targets = state_.config().xcp_targets;
    const auto* t = target();

    if (targets.empty()) {
        ImGui::TextColored(COL_WARN, "No XCP block configured.");
        ImGui::SameLine();
        ImGui::TextDisabled("Settings > Channels: device -> Baudrate -> XCP (right-click the canvas to add blocks)");
    } else {
        ImGui::SetNextItemWidth(em * 12);
        if (ImGui::BeginCombo("##target", t ? t->name.c_str() : "")) {
            for (const auto& o : targets)
                if (ImGui::Selectable(o.name.c_str(), t && o.name == t->name)) {
                    if (master_) master_.reset();
                    target_name_ = o.name;
                }
            ImGui::EndCombo();
        }
        if (t && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s, %s\nmaster -> slave 0x%X, slave -> master 0x%X\naddress extension %d, timeout %d ms",
                              t->interface.c_str(), t->fd ? "CAN FD" : "classic CAN", t->tx_id, t->rx_id, t->addr_ext,
                              t->timeout_ms);
        ImGui::SameLine();
        const auto st = master_ ? master_->state() : XcpMaster::State::Disconnected;
        const bool active = st == XcpMaster::State::Connected || st == XcpMaster::State::Connecting ||
                            st == XcpMaster::State::Error;
        if (ImGui::Button(active ? "Disconnect" : "Connect", ImVec2(em * 6.5f, 0))) {
            if (active) master_->disconnect();
            else connect();
        }
        ImGui::SameLine();
        const ImVec4 col = st == XcpMaster::State::Connected ? COL_OK : st == XcpMaster::State::Error ? COL_ERROR : COL_WARN;
        ImGui::TextColored(col, "%s", master_ ? master_->status().c_str() : "not connected");
        if (master_ && st == XcpMaster::State::Connected) {
            const auto info = master_->info();
            ImGui::SameLine();
            ImGui::TextDisabled("| %.0f samples/s%s%s", master_->rate_hz(), master_->daq_running() ? ", DAQ running" : "",
                                info.id.empty() ? "" : (", " + info.id).c_str());
        }
    }

    ImGui::SameLine(0, em * 2);
    if (ImGui::Button("Load ELF...")) open_elf_dialog();
    ImGui::SameLine();
    ImGui::BeginDisabled(elf_path_.empty());
    if (ImGui::Button("Reload")) load_elf(elf_path_);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Read the ELF again (after a new build)");
    ImGui::SameLine();
    if (elf_loading_.valid()) ImGui::TextDisabled("loading %s ...", elf_path_.c_str());
    else if (!elf_error_.empty()) ImGui::TextColored(COL_ERROR, "%s", elf_error_.c_str());
    else if (elf_) {
        ImGui::TextDisabled("%s (%zu scalars)", std::filesystem::path(elf_->path).filename().string().c_str(), elf_->leaf_count());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", elf_->path.c_str());
    } else ImGui::TextDisabled("no ELF loaded");

    ImGui::SameLine(0, em);
    if (ImGui::Button("Load A2L...")) open_a2l_dialog();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ASAP2 description (Simulink: model_build.m writes _BUILD/<model>_ert_rtw/<model>.a2l).\n"
                          "Variables, units and conversions come from the A2L, the addresses from the ELF if loaded.");
    if (!a2l_path_.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Reload##a2l")) load_a2l(a2l_path_);
        ImGui::SameLine();
        if (ImGui::SmallButton("x##a2l")) close_a2l();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close the A2L (variables from the ELF)");
        ImGui::SameLine();
        if (a2l_loading_.valid()) ImGui::TextDisabled("loading %s ...", a2l_path_.c_str());
        else if (!a2l_error_.empty()) ImGui::TextColored(COL_ERROR, "%s", a2l_error_.c_str());
        else if (image_) {
            ImGui::TextDisabled("%s (%zu scalars)", std::filesystem::path(a2l_path_).filename().string().c_str(), image_->leaf_count());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a2l_path_.c_str());
        }
    }

    ImGui::SameLine(0, em * 2);
    ImGui::SetNextItemWidth(em * 6);
    if (ImGui::InputInt("poll ms", &poll_ms_, 10, 100, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
        poll_ms_ = std::clamp(poll_ms_, 1, 10'000);
        push_measurements();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Period for polled variables (SHORT_UPLOAD, one request per variable).\n"
                          "DAQ variables are sent by the ECU in every cycle of their event.");
}

namespace {

const std::array<std::pair<const char*, double>, 10> WINDOW_PRESETS = {{
    {"1 s", 1}, {"2 s", 2}, {"5 s", 5}, {"10 s", 10}, {"30 s", 30},
    {"1 min", 60}, {"2 min", 120}, {"5 min", 300}, {"10 min", 600}, {"30 min", 1800},
}};

std::string format_window(double s)
{
    if (s >= 60.0) return std::format("{:.3g} min", s / 60.0);
    return std::format("{:.3g} s", s);
}

}   // namespace

void XcpView::reset_view()
{
    live_     = true;
    window_s_ = DEFAULT_WINDOW_S;
    for (auto& p : plots_) p.auto_y = true;
}

void XcpView::fit_all()
{
    std::lock_guard lock(data_mutex_);
    double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
    for (const auto& p : plots_)
        for (const auto& n : p.names)
            if (const auto it = data_.find(n); it != data_.end() && !it->second.t.empty()) {
                lo = std::min(lo, it->second.t.front());
                hi = std::max(hi, it->second.t.back());
            }
    if (lo >= hi) return;
    live_  = false;
    x_min_ = lo;
    x_max_ = hi;
    for (auto& p : plots_) p.fit_y = true;
}

void XcpView::render_plots()
{
    const float em = ImGui::GetFontSize();
    ImGuiIO&    io = ImGui::GetIO();

    // ── toolbar (like the Live graphs) ──
    const ImVec4 live_col = live_ ? ImVec4(0.20f, 0.60f, 0.25f, 1.0f) : ImVec4(0.80f, 0.50f, 0.10f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, live_col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(live_col.x * 1.15f, live_col.y * 1.15f, live_col.z * 1.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(live_col.x * 0.85f, live_col.y * 0.85f, live_col.z * 0.85f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button(live_ ? "Live" : "Paused", ImVec2(em * 5, 0))) live_ = !live_;
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(live_ ? "Following the newest data - click to pause" : "View is frozen - click to follow the newest data again");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 6);
    if (ImGui::BeginCombo("##window", format_window(window_s_).c_str())) {
        for (const auto& [label, secs] : WINDOW_PRESETS)
            if (ImGui::Selectable(label, std::abs(window_s_ - secs) < 1e-9)) {
                window_s_ = secs;
                live_     = true;
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Visible time window (also: mouse wheel in a plot while live)");
    ImGui::SameLine();
    bool all_auto = std::ranges::all_of(plots_, &Plot::auto_y);
    if (ImGui::Checkbox("Auto Y", &all_auto))
        for (auto& p : plots_) p.auto_y = all_auto;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scale the Y axes to the visible data (all plots).\nZooming / dragging a Y axis switches that plot to manual.");
    ImGui::SameLine();
    if (ImGui::Button("Fit")) fit_all();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show all recorded data");
    ImGui::SameLine();
    if (ImGui::Button("Default")) reset_view();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Back to the default view: live, %s window, Auto Y\n(also: double-click a plot)",
                          format_window(DEFAULT_WINDOW_S).c_str());
    ImGui::SameLine(0, em);
    if (ImGui::Button("+ Plot")) { plots_.emplace_back(); active_plot_ = plots_.size() - 1; }
    ImGui::SameLine();
    ImGui::BeginDisabled(plots_.size() < 2);
    if (ImGui::Button("- Plot")) {
        plots_.erase(plots_.begin() + static_cast<std::ptrdiff_t>(std::min(active_plot_, plots_.size() - 1)));
        active_plot_ = std::min(active_plot_, plots_.size() - 1);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Remove the selected plot (click a plot to select it)");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        std::lock_guard lock(data_mutex_);
        for (auto& [n, s] : data_) { s.t.clear(); s.v.clear(); }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Discard the recorded samples");
    ImGui::SameLine();
    ImGui::Checkbox("Log", &show_log_);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Drag variables from the right    into a plot (double-click = selected plot)\n"
                          "Mouse wheel                      zoom time window (live) / zoom (paused)\n"
                          "Left drag                        pan (pauses)\n"
                          "Right drag                       zoom to rectangle (pauses, Auto Y off)\n"
                          "Wheel / drag on the Y axis       zoom Y (switches Auto Y off for this plot)\n"
                          "Double-click                     default view\n"
                          "Right-click legend               remove / edit value");

    const float log_h  = show_log_ ? em * 7.0f : 0.0f;
    const float avail  = ImGui::GetContentRegionAvail().y - log_h;
    const std::size_t n = plots_.size();
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float h = std::max(em * 6.0f, (avail - spacing * static_cast<float>(n - 1)) / static_cast<float>(n));

    double latest = 0.0;
    {
        std::lock_guard lock(data_mutex_);
        latest = latest_t_;
    }

    // While live, the mouse wheel inside a plot changes the time window instead of zooming both axes.
    float saved_wheel = 0.0f;
    if (live_ && io.MouseWheel != 0.0f && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        const ImVec2 m = io.MousePos;
        for (const auto& p : plots_)
            if (m.x >= p.rect[0] && m.y >= p.rect[1] && m.x <= p.rect[2] && m.y <= p.rect[3]) {
                window_s_   = std::clamp(window_s_ * std::pow(0.85, static_cast<double>(io.MouseWheel)), 0.1, 7200.0);
                saved_wheel = io.MouseWheel;
                io.MouseWheel = 0.0f;
                break;
            }
    }
    if (live_) {
        x_max_ = std::max(latest, window_s_);   // until the window is full, t = 0 stays at the left edge
        x_min_ = x_max_ - window_s_;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) drag_in_plot_ = false;

    bool reset = false;
    if (ImGui::BeginChild("##xcp_plots", ImVec2(0, avail), ImGuiChildFlags_None)) {
        for (std::size_t i = 0; i < n; ++i) {
            auto& plot = plots_[i];
            ImGui::PushID(static_cast<int>(i));
            const bool selected = i == active_plot_ && n > 1;
            if (selected) ImPlot::PushStyleColor(ImPlotCol_PlotBorder, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

            // Y range of the visible data (Auto Y) / of all data (Fit)
            double y_lo = std::numeric_limits<double>::max(), y_hi = std::numeric_limits<double>::lowest();
            if (plot.auto_y || plot.fit_y) {
                std::lock_guard lock(data_mutex_);
                for (const auto& name : plot.names) {
                    const auto it = data_.find(name);
                    if (it == data_.end() || it->second.t.empty()) continue;
                    const auto& s  = it->second;
                    auto lo = std::ranges::lower_bound(s.t, x_min_);
                    auto hi = std::ranges::upper_bound(s.t, x_max_);
                    if (lo != s.t.begin()) --lo;   // the line enters the window from the previous sample
                    if (hi != s.t.end()) ++hi;
                    const auto b = s.v.begin() + (lo - s.t.begin()), e = s.v.begin() + (hi - s.t.begin());
                    for (auto v = b; v != e; ++v)
                        if (std::isfinite(*v)) { y_lo = std::min(y_lo, *v); y_hi = std::max(y_hi, *v); }
                }
            }

            if (ImPlot::BeginPlot("##xcp_plot", ImVec2(-1, h), ImPlotFlags_NoTitle)) {
                ImPlot::SetupAxes(i + 1 == n ? "t [s]" : nullptr, nullptr, i + 1 == n ? 0 : ImPlotAxisFlags_NoTickLabels, 0);
                ImPlot::SetupAxisLinks(ImAxis_X1, &x_min_, &x_max_);
                ImPlot::SetupLegend(ImPlotLocation_NorthWest);
                if ((plot.auto_y || plot.fit_y) && y_lo <= y_hi) {
                    const double span = y_hi - y_lo;
                    const double pad  = span > 0.0 ? span * 0.06 : std::max(std::abs(y_hi) * 0.1, 1.0);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, y_lo - pad, y_hi + pad, ImGuiCond_Always);
                }
                plot.fit_y = false;

                // ── interaction ──
                if (ImPlot::IsPlotHovered() && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
                    drag_in_plot_ = true;
                    active_plot_  = i;
                }
                if (drag_in_plot_ && ImPlot::IsPlotHovered()) {
                    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) live_ = false;
                    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) { live_ = false; plot.auto_y = false; }
                }
                if (ImPlot::IsPlotHovered() && io.MouseWheel != 0.0f) live_ = false;   // paused zoom (live: wheel eaten above)
                if (ImPlot::IsAxisHovered(ImAxis_Y1) && (io.MouseWheel != 0.0f || ImGui::IsMouseDragging(ImGuiMouseButton_Left)))
                    plot.auto_y = false;
                if (live_ && ImPlot::IsAxisHovered(ImAxis_X1) && io.MouseWheel != 0.0f)
                    window_s_ = std::clamp(window_s_ * std::pow(0.85, static_cast<double>(io.MouseWheel)), 0.1, 7200.0);
                if (ImPlot::IsPlotHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) reset = true;

                const float px = std::max(1.0f, ImPlot::GetPlotSize().x);
                std::string remove;
                {
                    std::lock_guard lock(data_mutex_);
                    for (const auto& name : plot.names) {
                        const auto        it    = data_.find(name);
                        const std::string label = display_name(name) + "###" + name;
                        if (it == data_.end() || it->second.t.empty()) {
                            ImPlot::PlotDummy(label.c_str());
                        } else {
                            const auto& s  = it->second;
                            auto lo = std::ranges::lower_bound(s.t, x_min_);
                            auto hi = std::ranges::upper_bound(s.t, x_max_);
                            if (lo != s.t.begin()) --lo;
                            if (hi != s.t.end()) ++hi;
                            const auto i0    = static_cast<std::size_t>(lo - s.t.begin());
                            const auto count = static_cast<std::size_t>(hi - lo);
                            const std::size_t stride = std::max<std::size_t>(1, count / static_cast<std::size_t>(px * 2));
                            const int pts = static_cast<int>((count + stride - 1) / stride);
                            if (pts > 0)
                                ImPlot::PlotLine(label.c_str(), s.t.data() + i0, s.v.data() + i0, pts, 0, 0,
                                                 static_cast<int>(stride * sizeof(double)));
                        }
                        if (ImPlot::BeginLegendPopup(label.c_str())) {
                            ImGui::TextUnformatted(display_name(name).c_str());
                            if (display_name(name) != name) ImGui::TextDisabled("%s", name.c_str());
                            ImGui::Separator();
                            if (ImGui::MenuItem("Remove from plot")) remove = name;
                            if (ImGui::MenuItem("Edit value...")) open_edit(name);
                            ImPlot::EndLegendPopup();
                        }
                    }

                    // ── cursor read-out: line + value of every variable at the mouse time ──
                    if (ImPlot::IsPlotHovered() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                        !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !plot.names.empty() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
                        const double t  = ImPlot::GetPlotMousePos(ImAxis_X1, ImAxis_Y1).x;
                        const float  cx = ImPlot::PlotToPixels(t, 0.0).x;
                        ImPlot::PushPlotClipRect();
                        ImPlot::GetPlotDrawList()->AddLine(ImVec2(cx, ImPlot::GetPlotPos().y),
                                                           ImVec2(cx, ImPlot::GetPlotPos().y + ImPlot::GetPlotSize().y),
                                                           IM_COL32(128, 128, 128, 160));
                        ImPlot::PopPlotClipRect();
                        ImGui::BeginTooltip();
                        ImGui::Text("t = %.3f s", t);
                        ImGui::Separator();
                        for (const auto& name : plot.names) {
                            const auto  it  = data_.find(name);
                            const auto* var = find(name);
                            if (it == data_.end() || it->second.t.empty()) {
                                ImGui::TextDisabled("%s: -", display_name(name).c_str());
                                continue;
                            }
                            const auto& s = it->second;
                            auto k = static_cast<std::size_t>(std::ranges::lower_bound(s.t, t) - s.t.begin());
                            if (k >= s.t.size()) k = s.t.size() - 1;
                            if (k > 0 && std::abs(s.t[k - 1] - t) < std::abs(s.t[k] - t)) --k;
                            ImGui::Text("%s: %s%s%s", display_name(name).c_str(), format_value(var, s.v[k]).c_str(),
                                        var && !var->unit.empty() ? " " : "", var ? var->unit.c_str() : "");
                        }
                        ImGui::EndTooltip();
                    }
                }
                if (!remove.empty()) std::erase(plot.names, remove);
                if (plot.names.empty()) {
                    const auto c = ImPlot::GetPlotPos();
                    const auto sz = ImPlot::GetPlotSize();
                    const char* hint = "drop variables here";
                    const ImVec2 ts = ImGui::CalcTextSize(hint);
                    ImPlot::GetPlotDrawList()->AddText(ImVec2(c.x + (sz.x - ts.x) * 0.5f, c.y + (sz.y - ts.y) * 0.5f),
                                                       ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
                }
                if (!plot.auto_y) {   // small marker: this plot has a manual Y range
                    const auto c = ImPlot::GetPlotPos();
                    const auto sz = ImPlot::GetPlotSize();
                    const char* tag = "manual Y";
                    const ImVec2 ts = ImGui::CalcTextSize(tag);
                    ImPlot::GetPlotDrawList()->AddText(ImVec2(c.x + sz.x - ts.x - em * 0.4f, c.y + em * 0.2f),
                                                       ImGui::GetColorU32(ImGuiCol_TextDisabled), tag);
                }

                if (ImPlot::BeginDragDropTargetPlot()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(DRAG_TYPE))
                        add_to_plot(std::string(static_cast<const char*>(p->Data)), i);
                    ImPlot::EndDragDropTarget();
                }
                const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
                plot.rect[0] = pos.x;
                plot.rect[1] = pos.y;
                plot.rect[2] = pos.x + size.x;
                plot.rect[3] = pos.y + size.y;
                ImPlot::EndPlot();
            }
            if (selected) ImPlot::PopStyleColor();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (saved_wheel != 0.0f) io.MouseWheel = saved_wheel;
    if (reset) reset_view();
    if (show_log_) render_log();
}

void XcpView::render_log()
{
    if (ImGui::BeginChild("##xcp_log", ImVec2(0, 0), ImGuiChildFlags_Border)) {
        for (const auto& l : log_) {
            const bool err = l.find("error") != std::string::npos || l.find("ERR_") != std::string::npos ||
                             l.find("timeout") != std::string::npos || l.find("down") != std::string::npos;
            if (err) ImGui::TextColored(COL_ERROR, "%s", l.c_str());
            else ImGui::TextUnformatted(l.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void XcpView::render_sidebar()
{
    const float em    = ImGui::GetFontSize();
    const float avail = ImGui::GetContentRegionAvail().y;
    const float top   = std::clamp(avail * measured_frac_, em * 5.0f, avail - em * 8.0f);

    ImGui::TextUnformatted("Measured");
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", measure_.size());
    if (ImGui::BeginChild("##measured", ImVec2(0, top - ImGui::GetFrameHeightWithSpacing()))) render_measured();
    ImGui::EndChild();

    // divider
    ImGui::InvisibleButton("##divider", ImVec2(-1, em * 0.4f));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) measured_frac_ = std::clamp(measured_frac_ + ImGui::GetIO().MouseDelta.y / avail, 0.1f, 0.9f);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y + em * 0.2f),
                                        ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMin().y + em * 0.2f),
                                        ImGui::GetColorU32(ImGuiCol_Separator));

    ImGui::TextUnformatted("Variables");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filter (e.g. ctrl.pid)", &filter_);
    if (ImGui::BeginChild("##tree")) render_tree();
    ImGui::EndChild();
}

void XcpView::render_measured()
{
    if (measure_.empty()) {
        ImGui::TextDisabled("Right-click a variable below -> Measure,\nor drag it into a plot.");
        return;
    }
    constexpr ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                   ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##meas", 4, tf)) return;
    const float em = ImGui::GetFontSize();
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Acquisition", ImGuiTableColumnFlags_WidthFixed, em * 7.5f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, em * 1.2f);
    ImGui::TableHeadersRow();

    std::string remove, edit;
    std::vector<XcpEventInfo> events;
    if (master_) events = master_->events();
    for (std::size_t i = 0; i < measure_.size(); ++i) {
        auto& m = measure_[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const ElfVariable* var  = find(m.name);
        const std::string  shown = display_name(m.name);
        if (!var && vars()) ImGui::TextColored(COL_ERROR, "%s", m.name.c_str());
        else ImGui::Selectable(shown.c_str(), selected_ == m.name, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
        if (ImGui::IsItemClicked()) selected_ = m.name;
        if (ImGui::IsItemHovered()) {
            if (!var && vars()) ImGui::SetTooltip("not in the loaded %s", image_ ? "A2L" : "ELF");
            else if (shown != m.name) ImGui::SetTooltip("%s", m.name.c_str());
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) edit = m.name;
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(DRAG_TYPE, m.name.c_str(), m.name.size() + 1);
            ImGui::TextUnformatted(m.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (var && ImGui::BeginPopupContextItem("##ctx")) {
            render_var_menu(*var);
            ImGui::EndPopup();
        }

        ImGui::TableNextColumn();
        {
            std::lock_guard lock(data_mutex_);
            const auto it = data_.find(m.name);
            if (it != data_.end() && it->second.has) {
                ImGui::TextUnformatted(format_value(var, it->second.last).c_str());
                if (var && !var->unit.empty()) {
                    ImGui::SameLine(0, ImGui::GetFontSize() * 0.3f);
                    ImGui::TextDisabled("%s", var->unit.c_str());
                }
            } else ImGui::TextDisabled("-");
        }

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        std::string preview = "poll";
        if (m.event >= 0) {
            preview = std::format("event {}", m.event);
            for (const auto& e : events)
                if (e.number == m.event) preview = e.name;
        }
        if (ImGui::BeginCombo("##acq", preview.c_str())) {
            if (ImGui::Selectable("poll", m.event < 0)) { m.event = -1; push_measurements(); }
            for (const auto& e : events)
                if (ImGui::Selectable(event_label(e.number).c_str(), m.event == e.number)) { m.event = e.number; push_measurements(); }
            if (events.empty()) ImGui::TextDisabled("connect to see the DAQ events");
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("poll: read every %d ms by the master\nDAQ: the ECU sends the value in every cycle of the event", poll_ms_);

        ImGui::TableNextColumn();
        if (ImGui::SmallButton("x")) remove = m.name;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop measuring (also removes it from the plots)");
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (!remove.empty()) unmeasure(remove);
    if (!edit.empty()) open_edit(edit);
}

void XcpView::render_var_menu(const ElfVariable& v)
{
    if (!v.description.empty()) ImGui::TextUnformatted(v.description.c_str());
    ImGui::TextUnformatted(v.path.c_str());
    if (v.type != XcpType::None || v.size)
        ImGui::TextDisabled("%s, %u byte(s) @ 0x%llX%s%s", v.type_name.c_str(), v.size, static_cast<unsigned long long>(v.address),
                            v.unit.empty() ? "" : ", unit ", v.unit.c_str());
    ImGui::Separator();
    if (!v.leaf()) {
        ImGui::TextDisabled(vars() && vars()->from_a2l && v.type_name.empty() ? "a block / group: open it and pick a variable"
                                                                             : "a struct / array: open it and pick a member");
        return;
    }
    const bool connected = master_ && master_->state() == XcpMaster::State::Connected;
    if (ImGui::MenuItem("Edit value...", nullptr, false, !v.read_only)) open_edit(v.path);
    if (v.read_only && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("read-only in the A2L (MEASUREMENT without READ_WRITE)");
    if (ImGui::MenuItem("Read value", nullptr, false, connected)) {
        const auto* t = target();
        master_->read(XcpVar::from(v, t ? static_cast<uint8_t>(t->addr_ext) : 0));
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Add to plot")) {
        for (std::size_t i = 0; i < plots_.size(); ++i)
            if (ImGui::MenuItem(std::format("Plot {}", i + 1).c_str())) add_to_plot(v.path, i);
        if (ImGui::MenuItem("New plot")) add_to_plot(v.path, plots_.size());
        ImGui::EndMenu();
    }
    const auto it = std::ranges::find(measure_, v.path, &XcpMeasureConfig::name);
    const int cur = it == measure_.end() ? -2 : it->event;
    if (ImGui::MenuItem("Measure (poll)", nullptr, cur == -1)) measure(v.path, -1);
    if (ImGui::BeginMenu("Measure every cycle (DAQ)", connected && master_->info().daq_dynamic)) {
        for (const auto& e : master_->events())
            if (ImGui::MenuItem(event_label(e.number).c_str(), nullptr, cur == e.number)) measure(v.path, e.number);
        ImGui::EndMenu();
    }
    if (!connected && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("connect first: the event channels come from the ECU");
    if (cur != -2 && ImGui::MenuItem("Stop measuring")) unmeasure(v.path);
    ImGui::Separator();
    if (ImGui::MenuItem("Copy name")) ImGui::SetClipboardText(v.path.c_str());
    if (!v.description.empty() && ImGui::MenuItem("Copy Simulink path")) ImGui::SetClipboardText(v.description.c_str());
}

void XcpView::render_tree()
{
    const ElfImage* img = vars();
    if (!img) {
        ImGui::TextDisabled(elf_loading_.valid() || a2l_loading_.valid()
                                ? "loading ..."
                                : "Load an ELF file (built with -g) or an A2L to see the variables.");
        return;
    }

    // filter: flat list of matching scalars
    if (filter_ != applied_filter_) {
        applied_filter_ = filter_;
        filtered_.clear();
        if (!filter_.empty()) {
            const std::string f = lower(filter_);
            for (const auto& [path, v] : img->by_path) {
                if (lower(path).find(f) == std::string::npos && lower(v->description).find(f) == std::string::npos) continue;
                filtered_.push_back(v);
                if (filtered_.size() >= MAX_FILTER) break;
            }
        }
    }

    std::vector<Row> rows;
    if (!filter_.empty()) {
        for (const auto* v : filtered_) rows.push_back({v, 0});
    } else {
        std::function<void(const ElfVariable&, int)> add = [&](const ElfVariable& v, int d) {
            rows.push_back({&v, d});
            if (!v.children.empty() && open_nodes_.contains(v.path))
                for (const auto& c : v.children) add(c, d + 1);
        };
        for (const auto& v : img->variables) add(v, 0);
    }

    constexpr ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                   ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##vars", 3, tf)) return;
    const float em = ImGui::GetFontSize();
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableHeadersRow();

    std::string edit, toggle;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const auto& row = rows[static_cast<std::size_t>(r)];
            const auto& v   = *row.var;
            ImGui::PushID(v.path.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(row.depth) * em * 0.9f);

            const bool has_children = !v.children.empty() && filter_.empty();
            ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (!has_children) fl |= ImGuiTreeNodeFlags_Leaf;
            if (selected_ == v.path) fl |= ImGuiTreeNodeFlags_Selected;
            if (has_children) ImGui::SetNextItemOpen(open_nodes_.contains(v.path));
            const std::string label = filter_.empty() ? v.name : v.description.empty() ? v.path : v.description;
            const bool is_measured = measured(v.path);
            if (is_measured) ImGui::PushStyleColor(ImGuiCol_Text, COL_OK);
            const bool now_open = ImGui::TreeNodeEx("##node", fl, "%s%s", label.c_str(), v.truncated ? " ..." : "");
            if (is_measured) ImGui::PopStyleColor();
            if (has_children && now_open != open_nodes_.contains(v.path)) toggle = v.path;
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selected_ = v.path;
            if (ImGui::IsItemHovered()) {
                if (v.leaf() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) add_to_plot(v.path, active_plot_);
                std::string tip = v.description.empty() ? v.path : v.description + "\n" + v.path;
                if (v.type != XcpType::None || v.size)
                    tip += std::format("\n{}, {} byte(s) @ 0x{:X}", v.type_name, v.size, v.address);
                if (v.bit_size) tip += std::format(", bits {}..{}", v.bit_offset, v.bit_offset + v.bit_size - 1);
                if (!v.unit.empty()) tip += "\nunit: " + v.unit;
                if (!v.conv.identity()) tip += std::format("\n{}", v.conv);
                if (v.leaf() && v.has_limits && (v.lower > -1e300 || v.upper < 1e300))
                    tip += std::format("\nlimits: {} .. {}", v.lower, v.upper);
                if (v.read_only) tip += "\nread-only (measurement)";
                if (v.truncated) tip += "\n(only the first elements are shown)";
                ImGui::SetTooltip("%s", tip.c_str());
            }
            if (v.leaf() && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(DRAG_TYPE, v.path.c_str(), v.path.size() + 1);
                ImGui::TextUnformatted(v.path.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("##ctx")) {
                render_var_menu(v);
                ImGui::EndPopup();
            }

            ImGui::TableNextColumn();
            if (v.leaf()) {
                std::lock_guard lock(data_mutex_);
                const auto it = data_.find(v.path);
                if (it != data_.end() && it->second.has) {
                    ImGui::TextUnformatted(format_value(&v, it->second.last).c_str());
                    if (!v.read_only && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) edit = v.path;
                }
            }
            ImGui::TableNextColumn();
            if (v.unit.empty()) ImGui::TextDisabled("%s", v.type_name.c_str());
            else ImGui::TextDisabled("%s [%s]", v.type_name.c_str(), v.unit.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    if (!toggle.empty()) {
        if (open_nodes_.contains(toggle)) open_nodes_.erase(toggle);
        else open_nodes_.insert(toggle);
    }
    if (!edit.empty()) open_edit(edit);
}

void XcpView::render_edit_popup()
{
    if (open_edit_) {
        ImGui::OpenPopup("Edit value##xcp");
        open_edit_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Edit value##xcp", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    const ElfVariable* var = find(edit_name_);
    if (var && !var->description.empty()) ImGui::TextUnformatted(var->description.c_str());
    ImGui::TextUnformatted(edit_name_.c_str());
    if (var) {
        ImGui::TextDisabled("%s, %u byte(s) @ 0x%llX%s", var->type_name.c_str(), var->size,
                            static_cast<unsigned long long>(var->address),
                            var->bit_size ? std::format(", bits {}..{}", var->bit_offset, var->bit_offset + var->bit_size - 1).c_str() : "");
        if (!var->unit.empty() || !var->conv.identity())
            ImGui::TextDisabled("%s%s%s", var->unit.empty() ? "" : ("unit " + var->unit).c_str(),
                                !var->unit.empty() && !var->conv.identity() ? ", " : "",
                                var->conv.identity() ? "" : std::format("{}", var->conv).c_str());
        if (var->read_only) ImGui::TextColored(COL_WARN, "read-only in the A2L (measurement)");
    } else {
        ImGui::TextColored(COL_ERROR, "not in the loaded %s", image_ ? "A2L" : "ELF");
    }
    {
        std::lock_guard lock(data_mutex_);
        const auto it = data_.find(edit_name_);
        if (it != data_.end() && it->second.has)
            ImGui::Text("ECU value: %s", format_value(var, it->second.last).c_str());
        else
            ImGui::TextDisabled("ECU value: not read yet");
    }
    ImGui::Separator();

    const bool connected = master_ && master_->state() == XcpMaster::State::Connected;
    bool write = false;
    if (var && !var->enumerators.empty()) {
        const auto cur = parse_value(edit_text_, var);
        std::string preview = edit_text_;
        for (const auto& [e, n] : var->enumerators)
            if (cur && static_cast<double>(e) == *cur) preview = n;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##enum", preview.c_str())) {
            for (const auto& [e, n] : var->enumerators)
                if (ImGui::Selectable(std::format("{} ({})", n, e).c_str(), cur && static_cast<double>(e) == *cur))
                    edit_text_ = std::to_string(e);
            ImGui::EndCombo();
        }
    } else if (var && var->type == XcpType::Bool) {
        bool b = parse_value(edit_text_, var).value_or(0.0) != 0.0;
        if (ImGui::Checkbox("value", &b)) edit_text_ = b ? "1" : "0";
    } else {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##value", "new value (12.5, -3, 0x1F, true)", &edit_text_,
                                     ImGuiInputTextFlags_EnterReturnsTrue))
            write = true;
    }
    if (!edit_error_.empty()) ImGui::TextColored(COL_ERROR, "%s", edit_error_.c_str());
    if (!connected) ImGui::TextColored(COL_WARN, "not connected");

    ImGui::BeginDisabled(!connected || !var);
    if (ImGui::Button("Write", ImVec2(ImGui::GetFontSize() * 6, 0))) write = true;
    ImGui::SameLine();
    if (ImGui::Button("Read")) {
        const auto* t = target();
        master_->read(XcpVar::from(*var, t ? static_cast<uint8_t>(t->addr_ext) : 0));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();

    if (write && connected && var) {
        const auto value = parse_value(edit_text_, var);
        if (!value) {
            edit_error_ = "not a number";
        } else if (var->has_limits && (*value < var->lower || *value > var->upper)) {
            edit_error_ = std::format("{} is outside the A2L limits [{}, {}]", *value, var->lower, var->upper);
        } else {
            try {
                const XcpVar xv = XcpVar::from(*var, target() ? static_cast<uint8_t>(target()->addr_ext) : 0);
                if (!xv.bit_size) (void)xcp_encode(*value, xv, false);   // range check here, not on the worker
                else {
                    XcpVar probe = xv;
                    (void)xcp_encode(*value, probe, false, std::vector<uint8_t>(xv.size, 0));
                }
                master_->write(xv, *value);
                edit_error_.clear();
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& e) {
                edit_error_ = e.what();
            }
        }
    }
    ImGui::EndPopup();
}
