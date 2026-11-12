#include "PipelineWindow.hpp"

#include "config/Pipeline.hpp"
#include "setup/CanDevices.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <imnodes.h>
#include "ImNodesPan.hpp"

namespace {

constexpr int ATTR_STRIDE = 1024;
constexpr int OUT_FLAG    = 512;

int  attr_id(int node, bool out, int port) { return node * ATTR_STRIDE + (out ? OUT_FLAG : 0) + port; }
int  attr_node(int attr) { return attr / ATTR_STRIDE; }
bool attr_is_out(int attr) { return (attr % ATTR_STRIDE) >= OUT_FLAG; }
int  attr_port(int attr) { return attr % OUT_FLAG; }

struct TypeInfo { const char* type; const char* label; ImU32 color; };
constexpr TypeInfo TYPES[] = {
    { "hw_socketcan",      "Hardware SocketCAN", IM_COL32(180, 100,  30, 255) },
    { "virtual_socketcan", "Virtual SocketCAN",  IM_COL32(150, 120,  30, 255) },
    { "baudrate",          "Baudrate",           IM_COL32( 40,  90, 160, 255) },
    { "decode",            "Decode (DBC)",       IM_COL32(120,  60, 150, 255) },
    { "xcp",               "XCP master",         IM_COL32(170,  50,  70, 255) },
    { "live",              "Live view",          IM_COL32( 40, 130,  60, 255) },
    { "log",               "Log",                IM_COL32( 90,  90,  95, 255) },
};

const TypeInfo& type_info(const std::string& t)
{
    for (const auto& i : TYPES)
        if (t == i.type) return i;
    return TYPES[0];
}

constexpr std::array<std::pair<const char*, uint32_t>, 5> BAUDRATES = {{
    {"125 kbit/s", 125'000}, {"250 kbit/s", 250'000}, {"500 kbit/s", 500'000},
    {"800 kbit/s", 800'000}, {"1 Mbit/s", 1'000'000},
}};

constexpr std::array<std::pair<const char*, uint32_t>, 5> DATA_BAUDRATES = {{
    {"1 Mbit/s", 1'000'000}, {"2 Mbit/s", 2'000'000}, {"4 Mbit/s", 4'000'000},
    {"5 Mbit/s", 5'000'000}, {"8 Mbit/s", 8'000'000},
}};

// "0x7F0" / "2032" -> id; false if not a number
bool parse_id(const std::string& s, uint32_t& out)
{
    try {
        std::size_t pos = 0;
        const unsigned long v = std::stoul(s, &pos, 0);
        if (pos != s.size() || v > 0x9FFFFFFFul) return false;
        out = static_cast<uint32_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

const ImVec4 COL_ERROR = ImVec4(0.85f, 0.20f, 0.10f, 1.0f);
const ImVec4 COL_OK    = ImVec4(0.15f, 0.60f, 0.20f, 1.0f);

}   // namespace

PipelineWindow::PipelineWindow(AppState& state)
    : state_(state)
{
    visible = false;
}

PipelineWindow::~PipelineWindow()
{
    if (ctx_) {
        ImNodes::SetCurrentContext(ctx_);
        if (editor_) ImNodes::EditorContextFree(editor_);
        ImNodes::DestroyContext(ctx_);
    }
}

std::ostream& operator<<(std::ostream& os, const PipelineWindow& w)
{
    return os << "PipelineWindow(" << w.state_.config().pipeline.nodes.size() << " blocks, "
              << w.state_.config().pipeline.links.size() << " links)";
}

void PipelineWindow::refresh_devices()
{
    if (devices_time_ >= 0.0 && ImGui::GetTime() - devices_time_ < 2.0) return;
    devices_time_ = ImGui::GetTime();
    devices_      = detect_can_devices();
}

PipelineNodeSpec* PipelineWindow::find(int id)
{
    auto& nodes = state_.config().pipeline.nodes;
    const auto it = std::ranges::find(nodes, id, &PipelineNodeSpec::id);
    return it == nodes.end() ? nullptr : &*it;
}

void PipelineWindow::remove_node(int id)
{
    auto& p = state_.config().pipeline;
    std::erase_if(p.nodes, [id](const PipelineNodeSpec& n) { return n.id == id; });
    std::erase_if(p.links, [id](const PipelineLinkSpec& l) { return l.from_node == id || l.to_node == id; });
}

// Replaces the channel (= output port) list; links of removed ports go away, the others keep their port.
void PipelineWindow::set_channels(PipelineNodeSpec& n, std::vector<std::string> channels)
{
    auto& links = state_.config().pipeline.links;
    const int count = static_cast<int>(channels.size());
    std::erase_if(links, [&](const PipelineLinkSpec& l) { return l.from_node == n.id && l.from_port >= count; });
    n.channels = std::move(channels);
}

void PipelineWindow::add_node(const std::string& type, float x, float y)
{
    auto& p = state_.config().pipeline;
    for (const auto& o : p.nodes) next_id_ = std::max(next_id_, o.id + 1);
    PipelineNodeSpec n;
    n.id = next_id_++;
    n.type = type;
    n.x = x;
    n.y = y;

    auto used = [&](const std::string& name) {
        return std::ranges::any_of(p.nodes, [&](const auto& o) { return std::ranges::contains(o.channels, name); });
    };
    if (type == "virtual_socketcan") {
        for (int i = 0;; ++i)
            if (const std::string name = std::format("vcan{}", i); !used(name)) { n.channels = {name}; break; }
    } else if (type == "hw_socketcan") {
        refresh_devices();
        for (const auto& d : devices_) {
            const bool taken = std::ranges::any_of(p.nodes, [&](const auto& o) { return o.device == d.id; });
            if (taken) continue;
            n.device   = d.id;
            n.channels = d.channels;
            break;
        }
        if (n.channels.empty())
            for (int i = 0;; ++i)
                if (const std::string name = std::format("can{}", i); !used(name)) { n.channels = {name}; break; }
    } else if (type == "log") {
        n.format = "mcap";
    } else if (type == "xcp") {
        const int count = static_cast<int>(std::ranges::count(p.nodes, std::string("xcp"), &PipelineNodeSpec::type));
        n.name = count == 0 ? "ECU" : std::format("ECU {}", count + 1);
    }
    p.nodes.push_back(std::move(n));
}

// Blocks grouped by role: inputs (devices), processing, outputs.
void PipelineWindow::render_block_menu()
{
    const auto item = [&](const char* type) {
        const auto& ti = type_info(type);
        if (ImGui::MenuItem(ti.label)) add_node(type, add_x_, add_y_);
    };
    ImGui::TextDisabled("Inputs");
    item("hw_socketcan");
    item("virtual_socketcan");
    ImGui::Separator();
    ImGui::TextDisabled("Processing");
    item("baudrate");
    item("decode");
    item("xcp");
    ImGui::Separator();
    ImGui::TextDisabled("Outputs");
    item("live");
    if (ImGui::MenuItem("Log (MCAP)")) add_node("log", add_x_, add_y_);
    if (ImGui::MenuItem("Log (BLF)")) {
        add_node("log", add_x_, add_y_);
        state_.config().pipeline.nodes.back().format = "blf";
    }
}

void PipelineWindow::render_add_popup()
{
    if (open_add_popup_) {
        ImGui::OpenPopup("##add_pipeline_block");
        open_add_popup_ = false;
    }
    if (!ImGui::BeginPopup("##add_pipeline_block")) return;
    ImGui::TextDisabled("Add block");
    ImGui::Separator();
    render_block_menu();
    ImGui::EndPopup();
}

void PipelineWindow::render_node(PipelineNodeSpec& n)
{
    const float em = ImGui::GetFontSize();
    const auto& ti = type_info(n.type);
    const bool is_hw = n.type == "hw_socketcan", is_virtual = n.type == "virtual_socketcan";

    ImNodes::PushColorStyle(ImNodesCol_TitleBar, ti.color);
    ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, ti.color + IM_COL32(25, 25, 25, 0));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, ti.color + IM_COL32(25, 25, 25, 0));
    ImNodes::BeginNode(n.id);
    ImGui::PushID(n.id);

    ImNodes::BeginNodeTitleBar();
    ImGui::TextUnformatted(n.type == "log" ? (n.format == "blf" ? "Log (BLF)" : "Log (MCAP)") : ti.label);
    ImNodes::EndNodeTitleBar();

    if (is_hw) {
        const auto cur = std::ranges::find(devices_, n.device, &CanDevice::id);
        const bool present = cur != devices_.end();
        std::string preview = "(choose by interface name)";
        if (!n.device.empty())
            preview = present ? cur->label : n.device + " (not connected)";
        ImGui::SetNextItemWidth(em * 17);
        if (ImGui::BeginCombo("##device", preview.c_str())) {
            if (ImGui::Selectable("(choose by interface name)", n.device.empty()))
                n.device.clear();
            if (devices_.empty())
                ImGui::TextDisabled("No CAN hardware found");
            for (const auto& d : devices_)
                if (ImGui::Selectable(d.label.c_str(), d.id == n.device)) {
                    n.device = d.id;
                    set_channels(n, d.channels);
                }
            ImGui::EndCombo();
        }
        if (!n.device.empty() && !present)
            ImGui::TextColored(COL_ERROR, "not connected");
        else if (present)
            ImGui::TextDisabled("%zu channel(s)", cur->channels.size());
        // follow a device whose interface names changed (re-plugged)
        if (present && cur->channels != n.channels && cur->channels.size() == n.channels.size())
            n.channels = cur->channels;
    }

    const bool editable = is_virtual || (is_hw && n.device.empty());
    if (editable) {
        std::vector<std::string> ch = n.channels;
        bool changed = false;
        if (ImGui::SmallButton("+ channel")) {
            ch.push_back(std::string(is_virtual ? "vcan" : "can") + std::to_string(ch.size()));
            changed = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(ch.empty());
        if (ImGui::SmallButton("- channel")) {
            ch.pop_back();
            changed = true;
        }
        ImGui::EndDisabled();
        if (changed) set_channels(n, std::move(ch));
    }

    if (n.type == "baudrate") {
        ImNodes::BeginInputAttribute(attr_id(n.id, false, 0));
        ImGui::TextUnformatted("in");
        ImNodes::EndInputAttribute();

        const char* preview = n.virtual_can ? "virtual" : "custom";
        if (!n.virtual_can)
            for (const auto& [label, rate] : BAUDRATES)
                if (rate == n.baudrate) preview = label;
        ImGui::SetNextItemWidth(em * 9);
        if (ImGui::BeginCombo("##baud", preview)) {
            if (ImGui::Selectable("virtual", n.virtual_can))
                n.virtual_can = true;
            for (const auto& [label, rate] : BAUDRATES)
                if (ImGui::Selectable(label, !n.virtual_can && n.baudrate == rate)) {
                    n.virtual_can = false;
                    n.baudrate    = rate;
                }
            if (ImGui::Selectable("custom", false)) {
                n.virtual_can = false;
                if (std::ranges::any_of(BAUDRATES, [&](const auto& b) { return b.second == n.baudrate; }))
                    n.baudrate = 100'000;   // a value that is not a preset shows the input field
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("virtual: no real bus, the interface is created as a vcan device");
        if (!n.virtual_can && std::string_view(preview) == "custom") {
            ImGui::SetNextItemWidth(em * 9);
            ImGui::InputScalar("bit/s", ImGuiDataType_U32, &n.baudrate);
        }
        ImGui::Checkbox("CAN FD", &n.fd);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("CAN FD: up to 64 bytes per frame, faster data phase (needs an FD capable adapter).\n"
                              "virtual: the vcan device gets MTU 72");
        if (n.fd && !n.virtual_can) {
            const char* dpreview = "custom";
            for (const auto& [label, rate] : DATA_BAUDRATES)
                if (rate == n.data_baudrate) dpreview = label;
            ImGui::SetNextItemWidth(em * 9);
            if (ImGui::BeginCombo("data##dbaud", dpreview)) {
                for (const auto& [label, rate] : DATA_BAUDRATES)
                    if (ImGui::Selectable(label, n.data_baudrate == rate)) n.data_baudrate = rate;
                if (ImGui::Selectable("custom", false) &&
                    std::ranges::any_of(DATA_BAUDRATES, [&](const auto& b) { return b.second == n.data_baudrate; }))
                    n.data_baudrate = 3'000'000;
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bit rate of the data phase (dbitrate)");
            if (std::string_view(dpreview) == "custom") {
                ImGui::SetNextItemWidth(em * 9);
                ImGui::InputScalar("data bit/s", ImGuiDataType_U32, &n.data_baudrate);
            }
        }
    } else if (n.type == "decode") {
        ImNodes::BeginInputAttribute(attr_id(n.id, false, 0));
        ImGui::TextUnformatted("in");
        ImNodes::EndInputAttribute();

        ImGui::SetNextItemWidth(em * 13);
        ImGui::InputTextWithHint("##dbc", "DBC file", &n.dbc);
        ImGui::SameLine();
        if (ImGui::SmallButton("...")) {
            browse_node_  = n.id;
            open_browser_ = true;
        }
        if (n.dbc.empty())
            ImGui::TextDisabled("no DBC: raw frames");
        else if (!std::filesystem::exists(expand_path(n.dbc)))
            ImGui::TextColored(COL_ERROR, "file not found");
        else
            ImGui::TextColored(COL_OK, "%s", expand_path(n.dbc).filename().string().c_str());
    } else if (n.type == "xcp") {
        ImNodes::BeginInputAttribute(attr_id(n.id, false, 0));
        ImGui::TextUnformatted("channel");
        ImNodes::EndInputAttribute();

        ImGui::SetNextItemWidth(em * 10);
        ImGui::InputTextWithHint("name", "ECU", &n.name);
        const auto id_field = [&](const char* label, uint32_t& id, const char* tip) {
            std::string text = std::format("0x{:X}", id);
            ImGui::SetNextItemWidth(em * 6);
            if (ImGui::InputText(label, &text, ImGuiInputTextFlags_CharsNoBlank)) {
                uint32_t v = 0;
                if (parse_id(text, v)) id = v;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n11-bit ID, values > 0x7FF (or bit 31 set) are 29-bit IDs", tip);
        };
        id_field("master -> ECU", n.xcp_tx_id, "CAN ID of the XCP commands (CRO), sent by caneo");
        id_field("ECU -> master", n.xcp_rx_id, "CAN ID of the responses and DAQ data (DTO), sent by the ECU");
        if (n.xcp_tx_id == n.xcp_rx_id) ImGui::TextColored(COL_ERROR, "both IDs are equal");
        ImGui::SetNextItemWidth(em * 6);
        ImGui::InputInt("timeout ms", &n.xcp_timeout_ms, 0);
        n.xcp_timeout_ms = std::clamp(n.xcp_timeout_ms, 5, 5000);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("XCP T1: how long to wait for a response");
        ImGui::SetNextItemWidth(em * 6);
        ImGui::InputInt("addr. ext.", &n.xcp_addr_ext, 0);
        n.xcp_addr_ext = std::clamp(n.xcp_addr_ext, 0, 255);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Address extension of all memory accesses (normally 0)");
        ImGui::Checkbox("pad frames", &n.xcp_pad);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pad every frame to 8 bytes (CAN FD: at least 8, then the next valid length).\n"
                              "Only for ECUs that require it (MAX_DLC_REQUIRED). XCPlite rejects a padded CONNECT\n"
                              "while XCP_ENABLE_PROTOCOL_LAYER_ETH is defined - leave it off there.");
        const auto& targets = state_.config().xcp_targets;
        const auto it = std::ranges::find_if(targets, [&](const auto& t) {
            return n.name.empty() ? t.name.starts_with("XCP ") : t.name == n.name;
        });
        if (it != targets.end())
            ImGui::TextColored(COL_OK, "%s%s", it->interface.c_str(), it->fd ? ", CAN FD" : ", classic CAN");
        else
            ImGui::TextDisabled("connect a channel (after Baudrate)");
    } else if (n.type == "live") {
        ImNodes::BeginInputAttribute(attr_id(n.id, false, 0));
        ImGui::TextUnformatted("trace / graphs");
        ImNodes::EndInputAttribute();
    } else if (n.type == "log") {
        ImNodes::BeginInputAttribute(attr_id(n.id, false, 0));
        ImGui::TextUnformatted("frames");
        ImNodes::EndInputAttribute();
        ImGui::SetNextItemWidth(em * 7);
        if (ImGui::BeginCombo("##fmt", n.format == "blf" ? "BLF" : "MCAP")) {
            if (ImGui::Selectable("MCAP", n.format != "blf")) n.format = "mcap";
            if (ImGui::Selectable("BLF", n.format == "blf")) n.format = "blf";
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("MCAP: decoded signals (needs a Decode block before)\nBLF: raw frames");
    }

    const int outs = pipeline_out_ports(n);
    for (int p = 0; p < outs; ++p) {
        ImNodes::BeginOutputAttribute(attr_id(n.id, true, p));
        if (!is_hw && !is_virtual) {   // baudrate / decode: one plain output
            ImGui::TextUnformatted("out");
            ImNodes::EndOutputAttribute();
            continue;
        }
        auto& name = n.channels[static_cast<std::size_t>(p)];
        if (editable) {
            ImGui::SetNextItemWidth(em * 7);
            ImGui::InputText("##ch", &name, ImGuiInputTextFlags_CharsNoBlank);
            ImGui::SameLine();
            ImGui::TextDisabled("ch %d", p);
        } else {
            ImGui::Text("ch %d: %s", p, name.c_str());
        }
        ImNodes::EndOutputAttribute();
    }

    ImGui::PopID();
    ImNodes::EndNode();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
}

void PipelineWindow::render()
{
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 60, ImGui::GetFontSize() * 30), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(title(), &visible))
        render_contents();
    ImGui::End();
}

void PipelineWindow::render_contents()
{
    if (!ctx_) {
        ctx_ = ImNodes::CreateContext();
        ImNodes::SetCurrentContext(ctx_);
        editor_ = ImNodes::EditorContextCreate();
        imnodes_setup_navigation();
        theme_.clear();
    }
    ImNodes::SetCurrentContext(ctx_);
    ImNodes::EditorContextSet(editor_);
    if (theme_ != state_.config().gui.theme) {
        theme_ = state_.config().gui.theme;
        if (theme_ == "light") ImNodes::StyleColorsLight();
        else                   ImNodes::StyleColorsDark();
    }

    auto& cfg = state_.config();
    auto& p   = cfg.pipeline;
    refresh_devices();

    if (last_path_ != state_.path()) {   // another config was loaded
        last_path_ = state_.path();
        synced_    = false;
    }
    if (!synced_) {   // ids of a loaded graph
        next_id_ = 1;
        for (const auto& n : p.nodes) next_id_ = std::max(next_id_, n.id + 1);
        last_ = p;
        synced_ = true;
        positions_dirty_ = true;
    }

    // toolbar
    if (ImGui::Button("+ Block"))
        ImGui::OpenPopup("##add_toolbar");
    if (ImGui::BeginPopup("##add_toolbar")) {
        add_x_ = 40.0f + 30.0f * static_cast<float>(p.nodes.size() % 8);
        add_y_ = 40.0f + 30.0f * static_cast<float>(p.nodes.size() % 8);
        render_block_menu();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Right-click: add block. Drag from a port to another to connect, Del removes the selection.");

    if (state_.channels_changed()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.5f, 0.0f, 1.0f));
        ImGui::TextUnformatted("Changes need a restart.");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Save & Restart") && state_.save())
            state_.request_restart();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            p = state_.running().pipeline;
            cfg.interfaces = state_.running().interfaces;
            cfg.xcp_targets = state_.running().xcp_targets;
            cfg.logging.format = state_.running().logging.format;
            synced_ = false;
        }
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    zoom_.toolbar(ImVec2(avail.x * 0.5f, avail.y * 0.5f));
    ImGui::SameLine();
    ImGui::TextDisabled("wheel = zoom, right-drag = pan");
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    zoom_.begin();
    ImNodes::BeginNodeEditor();
    zoom_.inside();
    for (auto& n : p.nodes) {
        if (positions_dirty_) ImNodes::SetNodeGridSpacePos(n.id, zoom_.to_grid(n.x, n.y));
        render_node(n);
    }
    positions_dirty_ = false;
    for (std::size_t i = 0; i < p.links.size(); ++i) {
        const auto& l = p.links[i];
        ImNodes::Link(static_cast<int>(i), attr_id(l.from_node, true, l.from_port), attr_id(l.to_node, false, l.to_port));
    }

    const bool hovered = ImNodes::IsEditorHovered();
    ImNodes::EndNodeEditor();
    zoom_.end();

    for (auto& n : p.nodes) {   // before navigate(): it may change the zoom
        const ImVec2 pos = zoom_.from_grid(ImNodes::GetNodeGridSpacePos(n.id));
        n.x = pos.x;
        n.y = pos.y;
    }
    if (zoom_.navigate(hovered, origin)) {
        const ImVec2 pan = ImNodes::EditorContextGetPanning();
        const ImVec2 mp  = ImGui::GetMousePos();
        add_x_ = (mp.x - origin.x - pan.x) / zoom_.zoom;
        add_y_ = (mp.y - origin.y - pan.y) / zoom_.zoom;
        open_add_popup_ = true;
    }
    if (zoom_.changed) positions_dirty_ = true;

    // new link: output -> input. Baudrate / decode inputs take one source, live / log any number.
    int a = 0, b = 0;
    if (ImNodes::IsLinkCreated(&a, &b)) {
        if (!attr_is_out(a)) std::swap(a, b);
        if (attr_is_out(a) && !attr_is_out(b) && attr_node(a) != attr_node(b)) {
            PipelineLinkSpec l{attr_node(a), attr_port(a), attr_node(b), attr_port(b)};
            const auto* target = find(l.to_node);
            const bool single = target && (target->type == "baudrate" || target->type == "decode" || target->type == "xcp");
            if (single)
                std::erase_if(p.links, [&](const auto& o) { return o.to_node == l.to_node && o.to_port == l.to_port; });
            std::erase_if(p.links, [&](const auto& o) { return o == l; });
            p.links.push_back(l);
        }
    }

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && hovered &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) &&
        !ImGui::GetIO().WantTextInput) {
        std::vector<int> links(static_cast<std::size_t>(ImNodes::NumSelectedLinks()));
        if (!links.empty()) {
            ImNodes::GetSelectedLinks(links.data());
            std::ranges::sort(links, std::greater<>{});
            for (int li : links)
                if (li >= 0 && li < static_cast<int>(p.links.size()))
                    p.links.erase(p.links.begin() + li);
            ImNodes::ClearLinkSelection();
        }
        std::vector<int> nodes(static_cast<std::size_t>(ImNodes::NumSelectedNodes()));
        if (!nodes.empty()) {
            ImNodes::GetSelectedNodes(nodes.data());
            for (int id : nodes) remove_node(id);
            ImNodes::ClearNodeSelection();
        }
    }

    render_add_popup();

    if (open_browser_) {
        open_browser_ = false;
        if (const auto* n = find(browse_node_))
            browser_.open("Select DBC", FileBrowser::Mode::Open, {".dbc"}, n->dbc.empty() ? std::filesystem::path{} : expand_path(n->dbc));
    }
    if (const auto chosen = browser_.render())
        if (auto* n = find(browse_node_)) n->dbc = chosen->string();

    // the interface list follows the graph (positions are not part of it)
    auto strip = [](PipelineSpec s) { for (auto& n : s.nodes) n.x = n.y = 0.0f; return s; };
    if (strip(p) != strip(last_)) {
        derive_interfaces(cfg);
        last_ = p;
    } else {
        last_ = p;
    }
}
