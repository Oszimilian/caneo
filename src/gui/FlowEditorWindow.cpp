#include "FlowEditorWindow.hpp"

#include "GraphWindow.hpp"
#include "flow/Flow.hpp"

#include <algorithm>
#include <format>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <imnodes.h>
#include "ImNodesPan.hpp"
#include <set>
#include <string_view>

namespace {

constexpr int ATTR_STRIDE = 1024;
constexpr int OUT_FLAG    = 512;

int attr_id(int node, bool out, int port) { return node * ATTR_STRIDE + (out ? OUT_FLAG : 0) + port; }
int attr_node(int attr) { return attr / ATTR_STRIDE; }
bool attr_is_out(int attr) { return (attr % ATTR_STRIDE) >= OUT_FLAG; }
int attr_port(int attr) { return attr % OUT_FLAG; }

const ImVec4 COL_ERROR  = ImVec4(0.85f, 0.20f, 0.10f, 1.0f);
const ImVec4 COL_ACTIVE = ImVec4(0.15f, 0.60f, 0.20f, 1.0f);

struct TypeInfo { const char* type; const char* label; ImU32 color; };
constexpr TypeInfo TYPES[] = {
    { "can_in",    "CAN In",    IM_COL32( 40,  90, 160, 255) },
    { "can_out",   "CAN Out",   IM_COL32(180, 100,  30, 255) },
    { "lua",       "Lua",       IM_COL32( 40, 130,  60, 255) },
    { "generator", "Generator", IM_COL32(120,  60, 150, 255) },
    { "scope",     "Scope",     IM_COL32( 90,  90,  95, 255) },
    { "const",     "Const",     IM_COL32(120,  60, 150, 255) },
    { "slider",    "Slider",    IM_COL32(120,  60, 150, 255) },
    { "button",    "Button",    IM_COL32(120,  60, 150, 255) },
    { "rs",        "RS Flip-Flop", IM_COL32( 30, 130, 140, 255) },
    { "switch",    "Switch",    IM_COL32( 30, 130, 140, 255) },
    { "add",       "Add",       IM_COL32(150, 120,  30, 255) },
    { "diff",      "Diff",      IM_COL32(150, 120,  30, 255) },
    { "lua_position", "Lua: Position (vx, vy, yaw rate)", IM_COL32( 40, 130,  60, 255) },
    { "mul",       "Multiply",  IM_COL32(150, 120,  30, 255) },
    { "sin",       "Sin",       IM_COL32(150, 120,  30, 255) },
    { "cos",       "Cos",       IM_COL32(150, 120,  30, 255) },
    { "gain",      "Gain",      IM_COL32(150, 120,  30, 255) },
    { "integrator", "1/s",      IM_COL32(150, 120,  30, 255) },
    { "compare",   "Compare",   IM_COL32(150, 120,  30, 255) },
    { "movavg",    "MovAvg",    IM_COL32( 30, 130, 140, 255) },
    { "fft",       "FFT",       IM_COL32( 30, 130, 140, 255) },
    { "csv",       "CSV Out",   IM_COL32( 90,  90,  95, 255) },
};

const TypeInfo& type_info(const std::string& type)
{
    for (const auto& t : TYPES)
        if (type == t.type) return t;
    return TYPES[0];
}

const char* const COMPARE_OPS[] = { "<", ">", ">=", "<=", "==" };
const char* const WAVES[] = { "sin", "pulse", "ramp", "sawtooth", "triangle", "noise", "const" };

FlowSpec logic_only(FlowSpec s)
{
    for (auto& n : s.nodes) {
        n.x = n.y = 0.0f;
        if (n.type == "slider") n.value = 0.0;   // moving a slider is not an edit that needs Apply
    }
    return s;
}

// The flows that run live on the CAN traffic (FlowEngine).
class LiveFlowBackend : public FlowBackend {
public:
    explicit LiveFlowBackend(AppState& state) : state_(state) {}

    std::vector<FlowInfo> snapshot() const override { return state_.flows().snapshot(); }
    void add(const FlowSpec& s) override { state_.flows().add(s); }
    void replace(std::size_t i, const FlowSpec& s) override { state_.flows().replace(i, s); }
    void remove(std::size_t i) override { state_.flows().remove(i); }
    void set_enabled(std::size_t i, bool on) override { state_.flows().set_enabled(i, on); }
    void trigger(std::size_t i, int node_id) override { state_.flows().trigger(i, node_id); }
    void set_control(std::size_t i, int node_id, double v) override { state_.flows().set_control(i, node_id, v); }

    std::vector<std::string> interfaces() const override
    {
        std::vector<std::string> out;
        for (const auto& i : state_.running().interfaces) out.push_back(i.name);
        return out;
    }

    const std::vector<Msg>& messages(const std::string& iface) override
    {
        const auto model = state_.models().model_for(iface);
        auto&      c     = cache_[iface];
        if (c.model != model) {   // DBC (re)loaded
            c.model = model;
            c.msgs.clear();
            if (model)
                for (const auto& m : model->messages()) {
                    Msg msg{m.name, {}};
                    for (const auto& sg : m.signals) msg.signals.push_back(sg.name);
                    c.msgs.push_back(std::move(msg));
                }
        }
        return c.msgs;
    }

private:
    struct Cache {
        std::shared_ptr<const SendModel> model;
        std::vector<Msg>                 msgs;
    };
    AppState&                    state_;
    std::map<std::string, Cache> cache_;
};

}   // namespace

FlowEditorWindow::FlowEditorWindow(AppState& state)
    : state_(state), backend_(std::make_unique<LiveFlowBackend>(state)), title_("Flow Editor")
{
    visible = false;
    // Save writes the diagram as shown, also edits that were not applied yet
    state_.set_flow_override([this](std::vector<FlowSpec>& flows) {
        if (!has_edit_ || edit_idx_ < 0 || edit_idx_ >= static_cast<int>(flows.size())) return;
        auto& f = flows[static_cast<std::size_t>(edit_idx_)];
        if (logic_only(edit_) == logic_only(f)) {
            // only node positions (or a slider) changed: the running flow stays, but save the layout as shown
            for (auto& n : f.nodes)
                for (const auto& e : edit_.nodes)
                    if (e.id == n.id) { n.x = e.x; n.y = e.y; }
            return;
        }
        FlowSpec spec = edit_;
        spec.enabled  = f.enabled;
        // slider values are live, keep the running ones
        for (auto& n : spec.nodes)
            if (n.type == "slider")
                for (const auto& o : f.nodes)
                    if (o.id == n.id && o.type == "slider") n.value = o.value;
        f = std::move(spec);
        state_.set_unapplied_note(std::format("flow '{}' has unapplied changes (saved, but not running - press Apply)", f.name));
    });
}

FlowEditorWindow::FlowEditorWindow(AppState& state, std::unique_ptr<FlowBackend> backend, std::string title)
    : state_(state), backend_(std::move(backend)), title_(std::move(title))
{
}

FlowEditorWindow::~FlowEditorWindow()
{
    if (ctx_) {
        ImNodes::SetCurrentContext(ctx_);
        if (editor_) ImNodes::EditorContextFree(editor_);
        ImNodes::DestroyContext(ctx_);
    }
}

// ─── flow selection / apply ─────────────────────────────────────────────────

void FlowEditorWindow::select(std::size_t idx)
{
    const auto flows = backend_->snapshot();
    if (idx >= flows.size()) return;
    edit_            = flows[idx].spec;
    applied_         = edit_;
    edit_idx_        = static_cast<int>(idx);
    has_edit_        = true;
    positions_dirty_ = true;
    next_id_         = 1;
    for (const auto& n : edit_.nodes) next_id_ = std::max(next_id_, n.id + 1);
}

void FlowEditorWindow::new_flow()
{
    edit_            = {};
    edit_.name       = std::format("flow {}", backend_->snapshot().size() + 1);
    applied_         = {};
    edit_idx_        = -1;
    has_edit_        = true;
    positions_dirty_ = true;
    next_id_         = 1;
}

void FlowEditorWindow::create_flow(FlowSpec spec)
{
    edit_            = std::move(spec);
    applied_         = {};
    edit_idx_        = -1;
    has_edit_        = true;
    positions_dirty_ = true;
    next_id_         = 1;
    for (const auto& n : edit_.nodes) next_id_ = std::max(next_id_, n.id + 1);
    apply();
}

void FlowEditorWindow::apply()
{
    if (edit_idx_ < 0) {
        edit_idx_ = static_cast<int>(backend_->snapshot().size());
        backend_->add(edit_);
    } else {
        backend_->replace(static_cast<std::size_t>(edit_idx_), edit_);
    }
    applied_ = edit_;
}

bool FlowEditorWindow::dirty(const FlowSpec& applied) const
{
    return edit_idx_ < 0 || logic_only(edit_) != logic_only(applied);
}

// ─── editing helpers ────────────────────────────────────────────────────────

int FlowEditorWindow::node_index(int id) const
{
    for (std::size_t i = 0; i < edit_.nodes.size(); ++i)
        if (edit_.nodes[i].id == id) return static_cast<int>(i);
    return -1;
}

void FlowEditorWindow::add_node(const std::string& type, float x, float y)
{
    FlowNodeSpec n;
    n.id   = next_id_++;
    n.type = type == "lua_position" ? "lua" : type;
    n.x    = x;
    n.y    = y;
    n.name = std::format("{} {}", type == "lua_position" ? "Position" : type_info(type).label, n.id);
    if (type == "can_in" || type == "can_out") {
        if (const auto ifaces = backend_->interfaces(); !ifaces.empty())
            n.interface = ifaces.front();
        n.name = type_info(type).label;
    } else if (type == "lua") {
        n.code = Flow::LUA_TEMPLATE;
    } else if (type == "lua_position") {
        n.code    = Flow::LUA_POSITION_TEMPLATE;
        n.inputs  = {"vx", "vy", "yaw_rate"};
        n.signals = {"X", "Y", "psi", "vX", "vY"};
    } else if (type == "scope") {
        n.name = std::format("scope{}", n.id);
    } else if (type == "rs") {
        n.op = "R";
    } else if (type == "slider") {
        n.name = "Slider";
    } else if (type == "csv") {
        n.signals = {"in1", "in2"};
        n.path    = "flow_out.csv";
    }
    edit_.nodes.push_back(std::move(n));
    positions_dirty_ = true;
}

void FlowEditorWindow::remove_node(int id)
{
    std::erase_if(edit_.nodes, [id](const FlowNodeSpec& n) { return n.id == id; });
    std::erase_if(edit_.links, [id](const FlowLinkSpec& l) { return l.from_node == id || l.to_node == id; });
}

// Replaces the signal (= port) list of a CAN block; links follow their signal.
void FlowEditorWindow::set_signals(FlowNodeSpec& node, std::vector<std::string> signals)
{
    const bool is_out = node.type == "can_in";   // can_in has output ports
    auto new_port = [&](int old_port) -> int {
        if (old_port < 0 || old_port >= static_cast<int>(node.signals.size())) return -1;
        const auto it = std::ranges::find(signals, node.signals[static_cast<std::size_t>(old_port)]);
        return it == signals.end() ? -1 : static_cast<int>(it - signals.begin());
    };
    std::vector<FlowLinkSpec> links;
    for (auto l : edit_.links) {
        int* port = nullptr;
        if (is_out && l.from_node == node.id) port = &l.from_port;
        if (!is_out && l.to_node == node.id)  port = &l.to_port;
        if (port) {
            *port = new_port(*port);
            if (*port < 0) continue;   // signal removed
        }
        links.push_back(l);
    }
    edit_.links  = std::move(links);
    if (node.type == "can_out") {   // constants follow their signals
        std::vector<double> values;
        for (const auto& s : signals) {
            const auto it = std::ranges::find(node.signals, s);
            const auto k  = static_cast<std::size_t>(it - node.signals.begin());
            values.push_back(it != node.signals.end() && k < node.sig_values.size() ? node.sig_values[k] : 0.0);
        }
        node.sig_values = std::move(values);
    }
    node.signals = std::move(signals);
}

bool FlowEditorWindow::msg_combo(FlowNodeSpec& node, const std::vector<FlowBackend::Msg>& msgs)
{
    bool changed = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::BeginCombo("##msg", node.msg.empty() ? "(message)" : node.msg.c_str())) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
            msg_filter_.clear();
        }
        ImGui::InputTextWithHint("##filter", "filter", &msg_filter_);
        std::string f = msg_filter_;
        std::ranges::transform(f, f.begin(), [](unsigned char c) { return std::tolower(c); });
        if (!msgs.empty()) {
            for (const auto& m : msgs) {
                std::string low = m.name;
                std::ranges::transform(low, low.begin(), [](unsigned char c) { return std::tolower(c); });
                if (!f.empty() && low.find(f) == std::string::npos) continue;
                if (ImGui::Selectable(m.name.c_str(), m.name == node.msg) && m.name != node.msg) {
                    node.msg = m.name;
                    set_signals(node, {});
                    node.name = node.msg;
                    changed = true;
                }
            }
        } else {
            ImGui::TextDisabled(backend_->offline() ? "no decoded messages for this interface" : "interface has no DBC");
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool FlowEditorWindow::signal_popup(FlowNodeSpec& node, const std::vector<FlowBackend::Msg>& msgs)
{
    bool changed = false;
    const std::string label = std::format("Signals ({})...", node.signals.size());
    if (ImGui::Button(label.c_str()))
        ImGui::OpenPopup("##signals");
    if (ImGui::BeginPopup("##signals")) {
        const auto mit = std::ranges::find(msgs, node.msg, &FlowBackend::Msg::name);
        if (mit == msgs.end()) {
            ImGui::TextDisabled("choose a message first");
        } else {
            const auto& msg = *mit;
            if (ImGui::SmallButton("all")) {
                set_signals(node, msg.signals);
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("none")) {
                set_signals(node, {});
                changed = true;
            }
            ImGui::Separator();
            for (const auto& name : msg.signals) {
                bool on = std::ranges::find(node.signals, name) != node.signals.end();
                if (ImGui::Checkbox(name.c_str(), &on)) {
                    // keep DBC order so ports do not jump around
                    std::vector<std::string> next;
                    for (const auto& o : msg.signals) {
                        const bool keep = o == name ? on : std::ranges::find(node.signals, o) != node.signals.end();
                        if (keep) next.push_back(o);
                    }
                    set_signals(node, std::move(next));
                    changed = true;
                }
            }
        }
        ImGui::EndPopup();
    }
    return changed;
}

// ─── rendering ──────────────────────────────────────────────────────────────

void FlowEditorWindow::render()
{
    if (!ImGui::Begin(title(), &visible)) {
        ImGui::End();
        return;
    }
    render_contents();
    ImGui::End();
}

void FlowEditorWindow::render_contents()
{
    if (!ctx_) {
        ctx_ = ImNodes::CreateContext();
        ImNodes::SetCurrentContext(ctx_);
        editor_ = ImNodes::EditorContextCreate();
        imnodes_setup_navigation();
        theme_.clear();
    }
    // Several editors exist (live window + Debug flow tabs): select both context and editor every frame.
    ImNodes::SetCurrentContext(ctx_);
    ImNodes::EditorContextSet(editor_);
    if (theme_ != state_.config().gui.theme) {
        theme_ = state_.config().gui.theme;
        if (theme_ == "light") ImNodes::StyleColorsLight();
        else                   ImNodes::StyleColorsDark();
    }

    const auto flows = backend_->snapshot();
    if (!has_edit_ && !flows.empty()) select(0);
    // the engine lost our flow (config loaded, deleted elsewhere)
    if (has_edit_ && edit_idx_ >= static_cast<int>(flows.size()) && edit_idx_ >= 0 && flows.empty())
        has_edit_ = false;

    render_toolbar(flows);
    if (has_edit_) {
        const FlowInfo* info = edit_idx_ >= 0 && edit_idx_ < static_cast<int>(flows.size())
                                   ? &flows[static_cast<std::size_t>(edit_idx_)] : nullptr;
        render_canvas(info);
    } else {
        ImGui::TextDisabled("No flow yet - press New.");
    }
}

void FlowEditorWindow::render_toolbar(const std::vector<FlowInfo>& flows)
{
    const float em = ImGui::GetFontSize();

    if (!compact_) {
        ImGui::SetNextItemWidth(em * 12);
        const std::string cur = !has_edit_ ? "" : edit_.name + (edit_idx_ < 0 ? " (new)" : "");
        if (ImGui::BeginCombo("##flow", cur.c_str())) {
            for (const auto& f : flows)
                if (ImGui::Selectable(f.spec.name.c_str(), static_cast<int>(f.idx) == edit_idx_))
                    select(f.idx);
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("New"))
            new_flow();
    }

    if (!has_edit_) return;

    if (!compact_) ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 9);
    ImGui::InputText("##name", &edit_.name);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 5);
    ImGui::InputInt("step ms", &edit_.step_ms, 0, 0);
    edit_.step_ms = std::max(1, edit_.step_ms);

    const FlowInfo* info = edit_idx_ >= 0 && edit_idx_ < static_cast<int>(flows.size())
                               ? &flows[static_cast<std::size_t>(edit_idx_)] : nullptr;
    if (info) edit_.enabled = applied_.enabled = info->spec.enabled;   // may be toggled in the flow list
    ImGui::SameLine();
    const bool is_dirty = dirty(info ? info->spec : applied_);
    if (is_dirty) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.50f, 0.10f, 1.0f));
    if (ImGui::Button("Apply"))
        apply();
    if (is_dirty) ImGui::PopStyleColor();
    if (is_dirty && !backend_->offline()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.85f, 0.5f, 0.05f, 1.0f), "not applied - new/changed blocks (scopes, plots) do nothing until Apply");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(backend_->offline() ? "Saves the diagram and runs all enabled flows over the loaded log"
                                              : "Creates or restarts the flow with the current diagram");

    if (info) {
        if (!backend_->offline()) {   // offline flows always run
            ImGui::SameLine();
            bool on = info->spec.enabled;
            if (ImGui::Checkbox("Run", &on)) {
                backend_->set_enabled(info->idx, on);
                edit_.enabled = applied_.enabled = on;
            }
        }
        if (!compact_) {
            ImGui::SameLine();
            if (ImGui::Button("Delete")) {
                backend_->remove(info->idx);
                has_edit_ = false;
                edit_idx_ = -1;
            }
        }
        const std::string err = info->state->log.error();
        ImGui::SameLine();
        if (!err.empty())
            ImGui::TextColored(COL_ERROR, "error: %s", err.c_str());
        else if (!info->loaded)
            ImGui::TextColored(COL_ERROR, "not loaded");
        else if (info->spec.enabled || backend_->offline())
            ImGui::TextColored(COL_ACTIVE, backend_->offline() ? "ok" : "running");
        else
            ImGui::TextDisabled("stopped");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Log", &show_log_);
}

void FlowEditorWindow::render_canvas(const FlowInfo* info)
{
    FlowState::Values live;
    if (info) live = info->state->values();
    const bool live_ok = info && live.out.size() == edit_.nodes.size();

    float log_h = 0.0f;
    if (show_log_) log_h = ImGui::GetTextLineHeightWithSpacing() * 7;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    zoom_.toolbar(ImVec2(avail.x * 0.5f, avail.y * 0.5f));
    ImGui::SameLine();
    ImGui::TextDisabled("wheel = zoom, right-drag = pan");
    ImGui::BeginChild("##canvas", ImVec2(0, -log_h), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    zoom_.begin();
    ImNodes::BeginNodeEditor();
    zoom_.inside();

    for (std::size_t i = 0; i < edit_.nodes.size(); ++i) {
        auto& node = edit_.nodes[i];
        if (positions_dirty_)
            ImNodes::SetNodeGridSpacePos(node.id, zoom_.to_grid(node.x, node.y));
        render_node(node, i, live_ok ? &live : nullptr);
    }
    positions_dirty_ = false;

    for (std::size_t i = 0; i < edit_.links.size(); ++i) {
        const auto& l = edit_.links[i];
        ImNodes::Link(static_cast<int>(i), attr_id(l.from_node, true, l.from_port), attr_id(l.to_node, false, l.to_port));
    }

    const bool hovered = ImNodes::IsEditorHovered();
    ImNodes::EndNodeEditor();
    zoom_.end();

    for (auto& n : edit_.nodes) {   // before navigate(): it may change the zoom
        const ImVec2 p = zoom_.from_grid(ImNodes::GetNodeGridSpacePos(n.id));
        n.x = p.x;
        n.y = p.y;
    }
    if (zoom_.navigate(hovered, origin)) {
        const ImVec2 pan = ImNodes::EditorContextGetPanning();
        const ImVec2 mp  = ImGui::GetMousePos();
        add_x_ = (mp.x - origin.x - pan.x) / zoom_.zoom;
        add_y_ = (mp.y - origin.y - pan.y) / zoom_.zoom;
        open_add_popup_ = true;
    }
    if (zoom_.changed) positions_dirty_ = true;

    // new link: normalise to output -> input; an input has one source
    int a = 0, b = 0;
    if (ImNodes::IsLinkCreated(&a, &b)) {
        if (!attr_is_out(a)) std::swap(a, b);
        if (attr_is_out(a) && !attr_is_out(b) && attr_node(a) != attr_node(b)) {
            FlowLinkSpec l{ attr_node(a), attr_port(a), attr_node(b), attr_port(b) };
            std::erase_if(edit_.links, [&](const FlowLinkSpec& o) { return o.to_node == l.to_node && o.to_port == l.to_port; });
            edit_.links.push_back(l);
        }
    }

    // Delete / Backspace removes the selection
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && hovered
        && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))
        && !ImGui::GetIO().WantTextInput) {
        std::vector<int> links(static_cast<std::size_t>(ImNodes::NumSelectedLinks()));
        if (!links.empty()) {
            ImNodes::GetSelectedLinks(links.data());
            std::ranges::sort(links, std::greater<>{});
            for (int li : links)
                if (li >= 0 && li < static_cast<int>(edit_.links.size()))
                    edit_.links.erase(edit_.links.begin() + li);
            ImNodes::ClearLinkSelection();
        }
        std::vector<int> nodes(static_cast<std::size_t>(ImNodes::NumSelectedNodes()));
        if (!nodes.empty()) {
            ImNodes::GetSelectedNodes(nodes.data());
            for (int id : nodes) remove_node(id);
            ImNodes::ClearNodeSelection();
        }
    }

    ImGui::EndChild();

    if (show_log_) {
        ImGui::BeginChild("##flowlog", ImVec2(0, 0), true);
        if (info)
            for (const auto& line : info->state->log.lines())
                ImGui::TextUnformatted(line.c_str());
        ImGui::EndChild();
    }

    render_add_popup();
    render_plot_popup();

    if (open_csv_browser_) {
        open_csv_browser_ = false;
        if (const int i = node_index(csv_browse_node_); i >= 0) {
            const auto& path = edit_.nodes[static_cast<std::size_t>(i)].path;
            csv_browser_.open("Export CSV", FileBrowser::Mode::Save, {".csv"}, expand_path(path.empty() ? "flow_out.csv" : path));
        }
    }
    if (const auto chosen = csv_browser_.render()) {
        if (const int i = node_index(csv_browse_node_); i >= 0) {
            auto p = *chosen;
            if (p.extension().empty()) p.replace_extension(".csv");
            edit_.nodes[static_cast<std::size_t>(i)].path = p.string();
        }
    }
}

void FlowEditorWindow::render_add_popup()
{
    if (open_add_popup_) {
        ImGui::OpenPopup("##add_block");
        open_add_popup_ = false;
    }
    if (ImGui::BeginPopup("##add_block")) {
        ImGui::TextDisabled("Add block");
        ImGui::Separator();
        // grouped: sources -> processing -> sinks
        struct Group { const char* title; std::initializer_list<std::string_view> types; };
        const Group groups[] = {
            { "Inputs",     { "can_in", "generator", "const", "slider", "button" } },
            { "Processing", { "lua", "lua_position", "add", "mul", "gain", "integrator", "sin", "cos", "diff", "compare", "movavg", "fft", "rs", "switch" } },
            { "Outputs",    { "can_out", "scope", "csv" } },
        };
        bool first = true;
        for (const auto& g : groups) {
            if (!first) ImGui::Separator();
            first = false;
            ImGui::TextDisabled("%s", g.title);
            for (const auto type : g.types) {
                if (backend_->offline() && type == "can_out") continue;
                if (!backend_->offline() && type == "csv") continue;
                for (const auto& t : TYPES)
                    if (type == t.type && ImGui::MenuItem(t.label))
                        add_node(t.type, add_x_, add_y_);
            }
        }
        ImGui::EndPopup();
    }
}

void FlowEditorWindow::render_plot_popup()
{
    if (open_plot_popup_) {
        ImGui::OpenPopup("##flow_plot");
        open_plot_popup_ = false;
    }
    if (!ImGui::BeginPopup("##flow_plot")) return;
    GraphWindow* gw = state_.graph_window();
    ImGui::TextUnformatted(plot_series_.c_str());
    ImGui::Separator();
    if (!gw) {
        ImGui::TextDisabled("no graph window");
    } else {
        const char* axes[] = { "Y1", "Y2", "Y3" };
        const auto  names  = gw->graph_names();
        for (std::size_t gi = 0; gi < names.size(); ++gi) {
            if (ImGui::BeginMenu(names[gi].c_str())) {
                for (int a = 0; a < 3; ++a)
                    if (ImGui::MenuItem(axes[a]))
                        gw->add_signal(gi, GraphWindow::GUARD_IFACE, 0, {}, plot_series_, 0.0, 0.0, a);
                ImGui::EndMenu();
            }
        }
        if (ImGui::BeginMenu("+ New Graph")) {
            for (int a = 0; a < 3; ++a)
                if (ImGui::MenuItem(axes[a])) {
                    const std::size_t gi = gw->add_graph();
                    gw->add_signal(gi, GraphWindow::GUARD_IFACE, 0, {}, plot_series_, 0.0, 0.0, a);
                }
            ImGui::EndMenu();
        }
    }
    ImGui::EndPopup();
}

void FlowEditorWindow::render_node(FlowNodeSpec& node, std::size_t idx, const FlowState::Values* live)
{
    const float em = ImGui::GetFontSize();
    const auto& ti = type_info(node.type);

    ImNodes::PushColorStyle(ImNodesCol_TitleBar, ti.color);
    ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, ti.color + IM_COL32(25, 25, 25, 0));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, ti.color + IM_COL32(25, 25, 25, 0));
    ImNodes::BeginNode(node.id);
    ImGui::PushID(node.id);

    ImNodes::BeginNodeTitleBar();
    ImGui::TextUnformatted(node.type == "can_in" || node.type == "can_out"
                               ? std::format("{}: {}", ti.label, node.msg.empty() ? "?" : node.msg).c_str()
                               : node.type == "scope" ? ti.label : node.name.c_str());
    ImNodes::EndNodeTitleBar();

    if (node.type == "can_in" || node.type == "can_out") {
        const auto ifaces = backend_->interfaces();
        ImGui::SetNextItemWidth(em * 12);
        if (ImGui::BeginCombo("##iface", node.interface.empty() ? "(interface)" : node.interface.c_str())) {
            for (const auto& i : ifaces)
                if (ImGui::Selectable(i.c_str(), i == node.interface) && i != node.interface) {
                    node.interface = i;
                    node.msg.clear();
                    set_signals(node, {});
                }
            ImGui::EndCombo();
        }
        const auto& msgs = backend_->messages(node.interface);
        msg_combo(node, msgs);
        signal_popup(node, msgs);
        if (node.type == "can_out") {
            ImGui::SetNextItemWidth(em * 6);
            ImGui::InputInt("period ms", &node.period_ms, 0, 0);
            node.period_ms = std::max(0, node.period_ms);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = send once when the flow starts");

            // constants for the inputs that are not connected
            node.sig_values.resize(node.signals.size(), 0.0);
            for (std::size_t k = 0; k < node.signals.size(); ++k) {
                const bool linked = std::ranges::any_of(edit_.links, [&](const FlowLinkSpec& l) {
                    return l.to_node == node.id && l.to_port == static_cast<int>(k);
                });
                ImGui::BeginDisabled(linked);
                ImGui::PushID(static_cast<int>(k));
                ImGui::SetNextItemWidth(em * 7);
                ImGui::InputDouble(node.signals[k].c_str(), &node.sig_values[k], 0, 0, "%.6g");
                ImGui::PopID();
                ImGui::EndDisabled();
            }
            if (!backend_->offline() && ImGui::Button("Send now")) {
                if (edit_idx_ >= 0) backend_->trigger(static_cast<std::size_t>(edit_idx_), node.id);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sends the message once (the flow has to be running)");
        }
    } else if (node.type == "lua") {
        {   // output names: "a, b" -> two output ports, step() returns {a = .., b = ..}
            std::string joined;
            for (const auto& s : node.signals) joined += (joined.empty() ? "" : ", ") + s;
            ImGui::SetNextItemWidth(em * 14);
            if (ImGui::InputTextWithHint("##outs", "outputs: a, b, c", &joined)) {
                std::vector<std::string> names;
                std::string              cur;
                for (const char c : joined + ",") {
                    if (c == ',') {
                        const auto b = cur.find_first_not_of(' '), e = cur.find_last_not_of(' ');
                        if (b != std::string::npos) names.push_back(cur.substr(b, e - b + 1));
                        cur.clear();
                    } else {
                        cur += c;
                    }
                }
                // links of removed outputs go away
                const int n = std::max<int>(1, static_cast<int>(names.size()));
                std::erase_if(edit_.links, [&](const FlowLinkSpec& l) { return l.from_node == node.id && l.from_port >= n; });
                node.signals = std::move(names);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Names of the outputs. step() may return a table {name = value} (or an array);\n"
                                  "send(msg, {sig = value}[, iface]) and send_raw(id, {bytes}[, iface]) send CAN frames.");
        }
        {   // input names: "vx, vy" -> step(vx, vy, t)
            std::string joined;
            for (const auto& s : node.inputs) joined += (joined.empty() ? "" : ", ") + s;
            ImGui::SetNextItemWidth(em * 14);
            if (ImGui::InputTextWithHint("##ins", "inputs: a, b, c (default u)", &joined)) {
                std::vector<std::string> names;
                std::string              cur;
                for (const char c : joined + ",") {
                    if (c == ',') {
                        const auto b = cur.find_first_not_of(' '), e = cur.find_last_not_of(' ');
                        if (b != std::string::npos) names.push_back(cur.substr(b, e - b + 1));
                        cur.clear();
                    } else {
                        cur += c;
                    }
                }
                const int n = std::max<int>(1, static_cast<int>(names.size()));
                std::erase_if(edit_.links, [&](const FlowLinkSpec& l) { return l.to_node == node.id && l.to_port >= n; });
                node.inputs = std::move(names);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Names of the inputs. One input: step(u, t); several: step(in1, in2, ..., t).");
        }
        ImGui::InputTextMultiline("##code", &node.code, ImVec2(em * 24, em * 11), ImGuiInputTextFlags_AllowTabInput);
    } else if (node.type == "generator") {
        ImGui::SetNextItemWidth(em * 8);
        if (ImGui::BeginCombo("wave", node.wave.c_str())) {
            for (const char* w : WAVES)
                if (ImGui::Selectable(w, node.wave == w)) node.wave = w;
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(em * 8);
        ImGui::InputDouble("amplitude", &node.amplitude, 0, 0, "%.4g");
        ImGui::SetNextItemWidth(em * 8);
        ImGui::InputDouble("offset", &node.offset, 0, 0, "%.4g");
        if (node.wave != "const" && node.wave != "noise") {
            ImGui::SetNextItemWidth(em * 8);
            ImGui::InputDouble("freq Hz", &node.frequency, 0, 0, "%.4g");
            ImGui::SetNextItemWidth(em * 8);
            ImGui::InputDouble("phase deg", &node.phase, 0, 0, "%.4g");
        }
        if (node.wave == "pulse") {
            ImGui::SetNextItemWidth(em * 8);
            ImGui::InputDouble("duty", &node.duty, 0, 0, "%.3g");
            node.duty = std::clamp(node.duty, 0.0, 1.0);
        }
    } else if (node.type == "const") {
        ImGui::SetNextItemWidth(em * 8);
        ImGui::InputDouble("value", &node.offset, 0, 0, "%.6g");
    } else if (node.type == "slider") {
        // live: every change goes straight into the running flow (no Apply)
        ImGui::SetNextItemWidth(em * 12);
        if (ImGui::SliderScalar("##value", ImGuiDataType_Double, &node.value, &node.min, &node.max, "%.4g")) {
            if (edit_idx_ >= 0) backend_->set_control(static_cast<std::size_t>(edit_idx_), node.id, node.value);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+click to type a value");
        ImGui::SetNextItemWidth(em * 5);
        ImGui::InputDouble("min", &node.min, 0, 0, "%.4g");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 5);
        ImGui::InputDouble("max", &node.max, 0, 0, "%.4g");
        if (node.max <= node.min) node.max = node.min + 1.0;
        node.value = std::clamp(node.value, node.min, node.max);
    } else if (node.type == "button") {
        if (ImGui::Button("Press", ImVec2(em * 6, 0)) && edit_idx_ >= 0)
            backend_->set_control(static_cast<std::size_t>(edit_idx_), node.id, 1.0);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Output is 1 for one step, then 0 again (not sent to the CAN bus)");
    } else if (node.type == "rs") {
        ImGui::SetNextItemWidth(em * 9);
        if (ImGui::BeginCombo("##rsdom", node.op == "S" ? "set dominant" : "reset dominant")) {
            if (ImGui::Selectable("reset dominant", node.op != "S")) node.op = "R";
            if (ImGui::Selectable("set dominant", node.op == "S")) node.op = "S";
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Which input wins when S and R are 1 at the same time");
    } else if (node.type == "switch") {
        ImGui::TextDisabled("ctrl > 0.5: b, else a");
    } else if (node.type == "compare") {
        ImGui::SetNextItemWidth(em * 5);
        if (ImGui::BeginCombo("a ? b", node.op.c_str())) {
            for (const char* o : COMPARE_OPS)
                if (ImGui::Selectable(o, node.op == o)) node.op = o;
            ImGui::EndCombo();
        }
    } else if (node.type == "sin" || node.type == "cos") {
        ImGui::Checkbox("degrees", &node.degrees);
    } else if (node.type == "gain") {
        ImGui::SetNextItemWidth(em * 8);
        ImGui::InputDouble("gain", &node.gain, 0, 0, "%.9g");
    } else if (node.type == "integrator") {
        ImGui::SetNextItemWidth(em * 8);
        ImGui::InputDouble("start value", &node.offset, 0, 0, "%.9g");
    } else if (node.type == "diff") {
        ImGui::Checkbox("divide by dt", &node.derivative);
    } else if (node.type == "movavg") {
        ImGui::SetNextItemWidth(em * 6);
        ImGui::InputInt("samples", &node.window, 0, 0);
        node.window = std::clamp(node.window, 1, 100000);
    } else if (node.type == "fft") {
        ImGui::SetNextItemWidth(em * 6);
        if (ImGui::BeginCombo("samples", std::to_string(node.window).c_str())) {
            for (int n = 16; n <= 8192; n <<= 1)
                if (ImGui::Selectable(std::to_string(n).c_str(), node.window == n)) node.window = n;
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("out: peak freq [Hz], amplitude");
    } else if (node.type == "csv") {
        ImGui::SetNextItemWidth(em * 11);
        ImGui::InputText("##csvpath", &node.path);
        ImGui::SameLine();
        if (ImGui::SmallButton("...")) {
            csv_browse_node_  = node.id;
            open_csv_browser_ = true;   // opened outside of the node, see render_canvas()
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("file");
        for (std::size_t k = 0; k < node.signals.size(); ++k) {
            ImGui::PushID(static_cast<int>(k));
            ImGui::SetNextItemWidth(em * 10);
            ImGui::InputText("##col", &node.signals[k]);
            ImGui::SameLine();
            ImGui::TextDisabled("col %zu", k + 1);
            ImGui::PopID();
        }
        if (ImGui::SmallButton("+ input"))
            node.signals.push_back("in" + std::to_string(node.signals.size() + 1));
        ImGui::SameLine();
        ImGui::BeginDisabled(node.signals.size() <= 1);
        if (ImGui::SmallButton("- input")) {
            node.signals.pop_back();
            const int last = static_cast<int>(node.signals.size());
            std::erase_if(edit_.links, [&](const FlowLinkSpec& l) { return l.to_node == node.id && l.to_port >= last; });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Export")) {
            csv_status_[node.id] = edit_idx_ < 0 ? "Apply the flow first"
                                                 : backend_->export_csv(static_cast<std::size_t>(edit_idx_), node.id, node.path);
        }
        if (const auto it = csv_status_.find(node.id); it != csv_status_.end())
            ImGui::TextDisabled("%s", it->second.c_str());
    } else if (node.type == "scope") {
        ImGui::SetNextItemWidth(em * 10);
        ImGui::InputText("##scope", &node.name);
        if (backend_->offline()) {
            ImGui::TextDisabled("-> flow/%s/%s   ('/' in the name = sub-folders)", edit_.name.c_str(), node.name.c_str());
        } else if (ImGui::Button("Plot...")) {
            plot_series_     = series_name(edit_.name, node.name);
            open_plot_popup_ = true;
        }
    }

    ImGui::Spacing();

    const int n_in = flow_in_ports(node), n_out = flow_out_ports(node);
    auto port_name = [&](int p, bool out) -> std::string {
        if (node.type == "can_in" || node.type == "can_out" || node.type == "csv")
            return p < static_cast<int>(node.signals.size()) ? node.signals[static_cast<std::size_t>(p)] : "?";
        if (node.type == "add" || node.type == "mul" || node.type == "compare") {
            if (!out) return p == 0 ? "a" : "b";
            return node.type == "add" ? "a+b" : node.type == "mul" ? "a*b" : "a" + node.op + "b";
        }
        if (node.type == "fft" && out) return p == 0 ? "f [Hz]" : "amp";
        if (node.type == "rs") return out ? (p == 0 ? "Q" : "!Q") : (p == 0 ? "S" : "R");
        if (node.type == "switch" && !out) return p == 0 ? "a (ctrl 0)" : p == 1 ? "b (ctrl 1)" : "ctrl";
        if (node.type == "lua" && !out && p < static_cast<int>(node.inputs.size()))
            return node.inputs[static_cast<std::size_t>(p)];
        if (node.type == "lua" && out && p < static_cast<int>(node.signals.size()))
            return node.signals[static_cast<std::size_t>(p)];
        return out ? "y" : "u";
    };
    for (int p = 0; p < n_in; ++p) {
        ImNodes::BeginInputAttribute(attr_id(node.id, false, p));
        if (live && idx < live->in.size() && p < static_cast<int>(live->in[idx].size()))
            ImGui::Text("%s  %.5g", port_name(p, false).c_str(), live->in[idx][static_cast<std::size_t>(p)]);
        else
            ImGui::TextUnformatted(port_name(p, false).c_str());
        ImNodes::EndInputAttribute();
    }
    for (int p = 0; p < n_out; ++p) {
        ImNodes::BeginOutputAttribute(attr_id(node.id, true, p));
        const std::string label = port_name(p, true);
        if (live && idx < live->out.size() && p < static_cast<int>(live->out[idx].size()))
            ImGui::Text("%.5g  %s", live->out[idx][static_cast<std::size_t>(p)], label.c_str());
        else
            ImGui::TextUnformatted(label.c_str());
        ImNodes::EndOutputAttribute();
    }

    ImGui::PopID();
    ImNodes::EndNode();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
}
