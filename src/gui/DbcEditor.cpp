#include "DbcEditor.hpp"

#include "AppState.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <imgui.h>
#include <optional>
#include <utility>
#include <imgui_stdlib.h>

namespace {

ImU32 signal_color(int idx, float alpha = 1.0f)
{
    return ImColor::HSV(std::fmod(static_cast<float>(idx) * 0.13f, 1.0f), 0.55f, 0.65f, alpha);
}

} // namespace

std::ostream& operator<<(std::ostream& os, const DbcEditor& ed)
{
    return os << "DbcEditor(" << ed.path_ << ", " << ed.doc_ << ")";
}

void DbcEditor::select_message(int idx)
{
    sel_msg_    = idx;
    sel_sig_    = -1;
    node_focus_ = false;
}

void DbcEditor::new_document()
{
    doc_   = DbcDoc{};
    network_.reset();
    path_.clear();
    dirty_ = false;
    sel_msg_ = sel_sig_ = sel_node_ = -1;
    node_focus_ = false;
    error_.clear();
}

void DbcEditor::remember(const std::string& path)
{
    auto& hist = state_.config().dbc_history;
    std::erase(hist, path);
    hist.insert(hist.begin(), path);
    constexpr std::size_t MAX_HISTORY = 20;
    if (hist.size() > MAX_HISTORY)
        hist.resize(MAX_HISTORY);
}

void DbcEditor::load_path(const std::string& path)
{
    try {
        doc_  = DbcDoc::load(path);
        path_ = path;
        network_.load(path_);
        dirty_ = false;
        error_.clear();
        sel_msg_    = doc_.messages.empty() ? -1 : 0;
        sel_sig_    = -1;
        sel_node_   = doc_.nodes.empty() ? -1 : 0;
        node_focus_ = false;
        remember(path);
    } catch (const std::exception& e) {
        error_ = e.what();
    }
}

void DbcEditor::open_path(const std::string& path)
{
    if (dirty_) {
        pending_open_  = path;
        ask_discard_   = true;
        after_discard_ = Browse::None;
    } else {
        load_path(path);
    }
}

void DbcEditor::render_history_items()
{
    auto& hist = state_.config().dbc_history;
    if (hist.empty()) {
        ImGui::TextDisabled("Empty - files are added when you open or save one");
        return;
    }
    std::optional<std::size_t> remove;
    std::optional<std::string> open;
    for (std::size_t i = 0; i < hist.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const auto name = std::filesystem::path(hist[i]).filename().string();
        if (ImGui::MenuItem(name.c_str()))
            open = hist[i];
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nRight-click: remove from history", hist[i].c_str());
        if (ImGui::BeginPopupContextItem("##hctx")) {
            if (ImGui::MenuItem("Remove from history"))
                remove = i;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (remove)
        hist.erase(hist.begin() + static_cast<std::ptrdiff_t>(*remove));
    if (open)
        open_path(*open);
}

void DbcEditor::save_to(const std::string& path)
{
    try {
        doc_.save(path);
        network_.save(path, doc_);
        path_  = path;
        dirty_ = false;
        error_.clear();
        remember(path);
    } catch (const std::exception& e) {
        error_ = e.what();
    }
}

void DbcEditor::request_import()
{
    if (dirty_) {
        ask_discard_   = true;
        after_discard_ = Browse::Import;
    } else {
        browse_ = Browse::Import;
    }
}

void DbcEditor::render()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const bool open = ImGui::Begin("DBC##view", nullptr, flags);
    ImGui::PopStyleVar(2);

    if (open) {
        render_toolbar();
        ImGui::Separator();

        const float em = ImGui::GetFontSize();
        if (ImGui::BeginTable("##dbc_layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("sidebar", ImGuiTableColumnFlags_WidthFixed, em * 24.0f);
            ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const float h = ImGui::GetContentRegionAvail().y;
            if (ImGui::BeginChild("##msgs", ImVec2(0, h * 0.45f)))
                render_messages();
            ImGui::EndChild();
            ImGui::Separator();
            if (ImGui::BeginChild("##nodes", ImVec2(0, 0)))
                render_nodes();
            ImGui::EndChild();

            ImGui::TableNextColumn();
            if (ImGui::BeginChild("##detail", ImVec2(0, 0))) {
                if (network_view_) {
                    const auto r = network_.render(doc_, sel_msg_, sel_node_);
                    dirty_ |= r.changed;
                    if (r.open_table) {
                        network_view_ = false;
                        node_focus_   = false;
                    }
                } else if (node_focus_ && sel_node_ >= 0 && sel_node_ < static_cast<int>(doc_.nodes.size()))
                    render_node(static_cast<std::size_t>(sel_node_));
                else if (sel_msg_ >= 0 && sel_msg_ < static_cast<int>(doc_.messages.size()))
                    render_message(doc_.messages[static_cast<std::size_t>(sel_msg_)]);
                else
                    ImGui::TextDisabled("Import a DBC or add a message / node on the left.");
            }
            ImGui::EndChild();
            ImGui::EndTable();
        }
        render_dialogs();
    }
    ImGui::End();
}

void DbcEditor::render_toolbar()
{
    if (ImGui::Button("New")) {
        if (dirty_) {
            ask_discard_   = true;
            after_discard_ = Browse::New;
        } else {
            new_document();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Import..."))
        request_import();
    ImGui::SameLine();
    if (ImGui::Button("History"))
        ImGui::OpenPopup("##dbchist");
    if (ImGui::BeginPopup("##dbchist")) {
        render_history_items();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        if (path_.empty())
            browse_ = Browse::SaveAs;
        else
            save_to(path_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save as..."))
        browse_ = Browse::SaveAs;
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    if (ImGui::RadioButton("Tables", !network_view_))
        network_view_ = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Network", network_view_))
        network_view_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", path_.empty() ? "(unsaved)" : path_.c_str(), dirty_ ? " *" : "");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    ImGui::InputTextWithHint("##version", "version", &doc_.version);

    const auto problems = doc_.problems();
    ImGui::SameLine();
    if (problems.empty()) {
        ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.3f, 1), "OK");
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.1f, 1), "%zu problem(s)", problems.size());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (std::size_t i = 0; i < problems.size() && i < 20; ++i)
                ImGui::TextUnformatted(problems[i].c_str());
            if (problems.size() > 20)
                ImGui::Text("... %zu more", problems.size() - 20);
            ImGui::EndTooltip();
        }
    }
    if (!error_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1), "%s", error_.c_str());
    }
}

void DbcEditor::render_messages()
{
    ImGui::TextUnformatted("Messages");
    ImGui::SameLine();
    if (ImGui::SmallButton("+")) {
        DbcMessage m;
        uint32_t   next = 0x100;
        for (const auto& o : doc_.messages)
            next = std::max(next, o.id + 1);
        m.id   = next;
        m.name = std::format("NewMessage_{}", doc_.messages.size() + 1);
        doc_.messages.push_back(std::move(m));
        select_message(static_cast<int>(doc_.messages.size()) - 1);
        dirty_ = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add message");
    ImGui::SameLine();
    if (ImGui::SmallButton("-") && sel_msg_ >= 0 && sel_msg_ < static_cast<int>(doc_.messages.size())) {
        doc_.messages.erase(doc_.messages.begin() + sel_msg_);
        select_message(std::min(sel_msg_, static_cast<int>(doc_.messages.size()) - 1));
        dirty_ = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Delete selected message");
    ImGui::SameLine();
    if (ImGui::SmallButton("sort")) {
        std::ranges::stable_sort(doc_.messages, {}, &DbcMessage::id);
        sel_msg_ = sel_sig_ = -1;
        dirty_   = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sort by ID");

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##mfilter", "filter", &filter_);

    std::string needle = filter_;
    std::ranges::transform(needle, needle.begin(), [](unsigned char c) { return std::tolower(c); });

    if (ImGui::BeginChild("##msglist")) {
        for (std::size_t i = 0; i < doc_.messages.size(); ++i) {
            const auto& m     = doc_.messages[i];
            const auto  label = std::format("0x{:X}  {}", m.id, m.name);
            if (!needle.empty()) {
                std::string l = label;
                std::ranges::transform(l, l.begin(), [](unsigned char c) { return std::tolower(c); });
                if (l.find(needle) == std::string::npos)
                    continue;
            }
            ImGui::PushID(static_cast<int>(i));
            // signals that belong to no node (no sender, no receiver) -> red
            const bool orphan = m.sender.empty() && std::ranges::any_of(m.signals, [](const DbcSignal& s) { return s.receivers.empty(); });
            if (orphan)
                ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(170, 40, 40, 110));
            if (orphan) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + ImGui::GetTextLineHeight()),
                                                          IM_COL32(170, 40, 40, 110));
            }
            if (ImGui::Selectable(label.c_str(), !node_focus_ && sel_msg_ == static_cast<int>(i)))
                select_message(static_cast<int>(i));
            if (orphan)
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%zu signals, %d bytes, sender: %s", m.signals.size(), m.size,
                                  m.sender.empty() ? "-" : m.sender.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void DbcEditor::render_nodes()
{
    ImGui::TextUnformatted("Nodes");
    ImGui::SameLine();
    if (ImGui::SmallButton("+")) {
        std::string name;
        for (std::size_t n = doc_.nodes.size() + 1;; ++n) {
            name = std::format("Node_{}", n);
            if (doc_.find_node(name) < 0)
                break;
        }
        doc_.nodes.push_back({name, {}});
        sel_node_   = static_cast<int>(doc_.nodes.size()) - 1;
        node_focus_ = true;
        dirty_      = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add node");
    ImGui::SameLine();
    if (ImGui::SmallButton("-") && sel_node_ >= 0 && sel_node_ < static_cast<int>(doc_.nodes.size())) {
        doc_.remove_node(static_cast<std::size_t>(sel_node_));
        sel_node_ = std::min(sel_node_, static_cast<int>(doc_.nodes.size()) - 1);
        dirty_    = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Delete selected node (senders / receivers are cleared)");

    if (ImGui::BeginChild("##nodelist")) {
        for (std::size_t i = 0; i < doc_.nodes.size(); ++i) {
            const std::string name = doc_.nodes[i].name;   // copy: the tree may edit doc_
            ImGui::PushID(static_cast<int>(i));
            const bool selected = node_focus_ && sel_node_ == static_cast<int>(i);
            const bool open = ImGui::TreeNodeEx("##node", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                                              (selected ? ImGuiTreeNodeFlags_Selected : 0),
                                                "%s", name.c_str());
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                sel_node_   = static_cast<int>(i);
                node_focus_ = true;
            }
            if (ImGui::BeginPopupContextItem("##nodectx")) {
                if (ImGui::MenuItem("Delete node")) {
                    doc_.remove_node(i);
                    sel_node_ = std::min(sel_node_, static_cast<int>(doc_.nodes.size()) - 1);
                    dirty_    = true;
                    ImGui::EndPopup();
                    ImGui::PopID();
                    if (open)
                        ImGui::TreePop();
                    break;
                }
                ImGui::EndPopup();
            }
            if (open) {
                render_node_tree(name);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

// Children of a node in the tree: the messages it sends (with their signals).
void DbcEditor::render_node_tree(const std::string& node)
{
    const auto open_signal = [&](std::size_t mi, int si) {
        select_message(static_cast<int>(mi));
        sel_sig_ = si;
    };
    const auto add_btn = [](const char* id, const char* tip) {
        ImGui::SameLine();
        const bool r = ImGui::SmallButton(id);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tip);
        return r;
    };

    // --- sends
    const bool tx_open = ImGui::TreeNodeEx("Sends", ImGuiTreeNodeFlags_DefaultOpen);
    if (add_btn("+##tx", "Add a message sent by this node"))
        ImGui::OpenPopup("##addtx");
    if (ImGui::BeginPopup("##addtx")) {
        if (ImGui::MenuItem("New message")) {
            DbcMessage m;
            uint32_t   next = 0x100;
            for (const auto& o : doc_.messages)
                next = std::max(next, o.id + 1);
            m.id     = next;
            m.name   = std::format("NewMessage_{}", doc_.messages.size() + 1);
            m.sender = node;
            doc_.messages.push_back(std::move(m));
            dirty_ = true;
        }
        ImGui::Separator();
        for (auto& m : doc_.messages)
            if (m.sender != node && ImGui::MenuItem(m.sender.empty() ? m.name.c_str() : (m.name + "  (from " + m.sender + ")").c_str())) {
                m.sender = node;
                dirty_   = true;
            }
        ImGui::EndPopup();
    }
    if (tx_open) {
        int unassign = -1;
        for (std::size_t mi = 0; mi < doc_.messages.size(); ++mi) {
            const auto& m = doc_.messages[mi];
            if (m.sender != node)
                continue;
            ImGui::PushID(static_cast<int>(mi));
            const bool o = ImGui::TreeNodeEx("##m", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap, "%s", m.name.c_str());
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                select_message(static_cast<int>(mi));
            if (add_btn("x##txm", "Remove from this node (message stays)"))
                unassign = static_cast<int>(mi);
            if (o) {
                for (std::size_t si = 0; si < m.signals.size(); ++si) {
                    ImGui::PushID(static_cast<int>(si));
                    ImGui::TreeNodeEx("##s", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                      "%s", m.signals[si].name.c_str());
                    if (ImGui::IsItemClicked())
                        open_signal(mi, static_cast<int>(si));
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (unassign >= 0) {
            doc_.messages[static_cast<std::size_t>(unassign)].sender.clear();
            dirty_ = true;
        }
        ImGui::TreePop();
    }

    // --- receives: whole messages (= every signal of them)
    const bool rx_open = ImGui::TreeNodeEx("Receives", ImGuiTreeNodeFlags_DefaultOpen);
    if (add_btn("+##rx", "Receive a whole message (all its signals)"))
        ImGui::OpenPopup("##addrx");
    const auto receives = [&](const DbcMessage& m) {
        return !m.signals.empty() && std::ranges::all_of(m.signals, [&](const DbcSignal& s) {
            return std::ranges::find(s.receivers, node) != s.receivers.end();
        });
    };
    if (ImGui::BeginPopup("##addrx")) {
        for (auto& m : doc_.messages)
            if (!receives(m) && !m.signals.empty() && ImGui::MenuItem(m.name.c_str())) {
                for (auto& s : m.signals)
                    if (std::ranges::find(s.receivers, node) == s.receivers.end())
                        s.receivers.push_back(node);
                dirty_ = true;
            }
        ImGui::EndPopup();
    }
    if (rx_open) {
        for (std::size_t mi = 0; mi < doc_.messages.size(); ++mi) {
            auto& m = doc_.messages[mi];
            const bool any = std::ranges::any_of(m.signals, [&](const DbcSignal& s) {
                return std::ranges::find(s.receivers, node) != s.receivers.end();
            });
            if (!any)
                continue;
            ImGui::PushID(static_cast<int>(mi));
            ImGui::TreeNodeEx("##m", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
                                         ImGuiTreeNodeFlags_AllowOverlap,
                              "%s%s", m.name.c_str(), receives(m) ? "" : "  (some signals)");
            if (ImGui::IsItemClicked())
                select_message(static_cast<int>(mi));
            if (add_btn("x##rxm", "Stop receiving this message")) {
                for (auto& s : m.signals)
                    std::erase(s.receivers, node);
                dirty_ = true;
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}

void DbcEditor::render_node(std::size_t idx)
{
    DbcNode& n = doc_.nodes[idx];
    ImGui::Text("Node");
    ImGui::Separator();

    std::string name = n.name;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    if (ImGui::InputText("Name", &name) && !name.empty()) {
        doc_.rename_node(idx, name);
        dirty_ = true;
    }
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 6.0f);
    dirty_ |= ImGui::InputText("Comment", &n.comment);

    ImGui::Spacing();
    ImGui::TextUnformatted("Messages (Sender = this node transmits the message)");
    if (ImGui::BeginTable("##node_msgs", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6);
        ImGui::TableSetupColumn("Sender", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5);
        ImGui::TableSetupColumn("Receives signals", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < doc_.messages.size(); ++i) {
            auto& m = doc_.messages[i];
            int   rx = 0;
            for (const auto& s : m.signals)
                rx += std::ranges::find(s.receivers, n.name) != s.receivers.end();
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(m.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                select_message(static_cast<int>(i));
            ImGui::TableNextColumn();
            ImGui::Text("0x%X", m.id);
            ImGui::TableNextColumn();
            bool tx = m.sender == n.name;
            if (ImGui::Checkbox("##tx", &tx)) {
                m.sender = tx ? n.name : std::string();
                dirty_   = true;
            }
            ImGui::TableNextColumn();
            ImGui::Text("%d / %zu", rx, m.signals.size());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void DbcEditor::render_message(DbcMessage& m)
{
    const float em = ImGui::GetFontSize();
    ImGui::Text("Message");
    ImGui::Separator();

    ImGui::SetNextItemWidth(em * 16.0f);
    dirty_ |= ImGui::InputText("Name", &m.name);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 8.0f);
    dirty_ |= ImGui::InputScalar("ID (hex)", ImGuiDataType_U32, &m.id, nullptr, nullptr, "%X",
                                 ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    if (ImGui::Checkbox("Extended", &m.extended))
        dirty_ = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 7.0f);
    if (ImGui::InputInt("Bytes", &m.size)) {
        m.size = std::clamp(m.size, 1, 64);
        dirty_ = true;
    }

    // sender = node of this message
    ImGui::SetNextItemWidth(em * 16.0f);
    if (ImGui::BeginCombo("Sender node", m.sender.empty() ? "(none)" : m.sender.c_str())) {
        if (ImGui::Selectable("(none)", m.sender.empty())) {
            m.sender.clear();
            dirty_ = true;
        }
        for (const auto& n : doc_.nodes)
            if (ImGui::Selectable(n.name.c_str(), m.sender == n.name)) {
                m.sender = n.name;
                dirty_   = true;
            }
        ImGui::EndCombo();
    }
    if (doc_.nodes.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("Create nodes in the list on the lower left first");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-em * 5.0f);
    dirty_ |= ImGui::InputText("Comment", &m.comment);

    ImGui::Spacing();
    render_bit_layout(m);
    ImGui::Spacing();
    render_signal_table(m);
    ImGui::Spacing();
    if (sel_sig_ >= 0 && sel_sig_ < static_cast<int>(m.signals.size()))
        render_signal(m, m.signals[static_cast<std::size_t>(sel_sig_)]);
    else
        ImGui::TextDisabled("Select a signal to edit it.");
}

void DbcEditor::render_bit_layout(DbcMessage& m)
{
    const float em     = ImGui::GetFontSize();
    const float cw     = em * 4.2f, ch = em * 1.5f, label_w = em * 2.5f;
    const int   rows   = std::max(m.size, 1);
    const float height = std::min(rows, 8) * ch + ch + ImGui::GetStyle().ScrollbarSize;

    const auto cells = m.layout();
    ImDrawList* dl   = nullptr;

    if (!ImGui::BeginChild("##bits", ImVec2(0, height + em * 0.5f), ImGuiChildFlags_None,
                           ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::EndChild();
        return;
    }
    dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    for (int bit = 0; bit < 8; ++bit) {
        const auto s = std::to_string(7 - bit);
        dl->AddText(ImVec2(origin.x + label_w + bit * cw + cw * 0.5f - em * 0.25f, origin.y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    s.c_str());
    }
    for (int byte = 0; byte < rows; ++byte) {
        const auto s = std::format("B{}", byte);
        dl->AddText(ImVec2(origin.x, origin.y + ch + byte * ch + 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), s.c_str());
        for (int bit = 7; bit >= 0; --bit) {
            const int   pos = byte * 8 + bit;
            const int   c   = pos < static_cast<int>(cells.size()) ? cells[static_cast<std::size_t>(pos)] : -1;   // size 0 messages have no cells
            const ImVec2 a(origin.x + label_w + (7 - bit) * cw, origin.y + ch + byte * ch);
            const ImVec2 b(a.x + cw - 1, a.y + ch - 1);
            ImU32 col = ImGui::GetColorU32(ImGuiCol_FrameBg);
            if (c >= 0)
                col = signal_color(c);
            else if (c == -2)
                col = IM_COL32(200, 40, 40, 255);
            dl->AddRectFilled(a, b, col);
            if (c >= 0 && c == sel_sig_)
                dl->AddRect(a, b, IM_COL32(255, 255, 255, 255), 0, 0, 2.0f);

            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(pos);
            ImGui::InvisibleButton("##cell", ImVec2(cw - 1, ch - 1));
            ImGui::PopID();
            if (ImGui::IsItemClicked() && c >= 0)
                sel_sig_ = c;
            if (ImGui::IsItemHovered()) {
                if (c >= 0)
                    ImGui::SetTooltip("%s (bit %d)", m.signals[static_cast<std::size_t>(c)].name.c_str(), pos);
                else if (c == -2)
                    ImGui::SetTooltip("overlap (bit %d)", pos);
                else
                    ImGui::SetTooltip("free (bit %d)", pos);
            }
        }
    }
    // names at the start bit of each signal
    for (std::size_t i = 0; i < m.signals.size(); ++i) {
        const int sb = m.signals[i].start_bit;
        if (sb < 0 || sb >= rows * 8)
            continue;
        const ImVec2 a(origin.x + label_w + (7 - sb % 8) * cw, origin.y + ch + (sb / 8) * ch);
        dl->PushClipRect(a, ImVec2(a.x + cw - 1, a.y + ch - 1), true);
        dl->AddText(ImVec2(a.x + 2, a.y + (ch - em) * 0.5f), IM_COL32(255, 255, 255, 255), m.signals[i].name.c_str());
        dl->PopClipRect();
    }
    ImGui::EndChild();
}

void DbcEditor::render_signal_table(DbcMessage& m)
{
    ImGui::Text("Signals");
    ImGui::SameLine();
    if (ImGui::SmallButton("+##sig")) {
        DbcSignal s;
        s.name = std::format("Signal_{}", m.signals.size() + 1);
        const auto cells = m.layout();
        const auto free  = std::ranges::find(cells, -1);
        s.start_bit      = free == cells.end() ? 0 : static_cast<int>(free - cells.begin());
        s.bit_size       = std::min(8, std::max(1, static_cast<int>(cells.size()) - s.start_bit));
        s.max            = std::pow(2.0, s.bit_size) - 1;
        m.signals.push_back(std::move(s));
        sel_sig_ = static_cast<int>(m.signals.size()) - 1;
        dirty_   = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add signal at the first free bit");
    ImGui::SameLine();
    const bool have = sel_sig_ >= 0 && sel_sig_ < static_cast<int>(m.signals.size());
    if (ImGui::SmallButton("-##sig") && have) {
        m.signals.erase(m.signals.begin() + sel_sig_);
        sel_sig_ = std::min(sel_sig_, static_cast<int>(m.signals.size()) - 1);
        dirty_   = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("copy") && have) {
        DbcSignal c = m.signals[static_cast<std::size_t>(sel_sig_)];
        c.name += "_copy";
        m.signals.insert(m.signals.begin() + sel_sig_ + 1, std::move(c));
        ++sel_sig_;
        dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("up") && have && sel_sig_ > 0) {
        std::swap(m.signals[static_cast<std::size_t>(sel_sig_)], m.signals[static_cast<std::size_t>(sel_sig_ - 1)]);
        --sel_sig_;
        dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("down") && have && sel_sig_ + 1 < static_cast<int>(m.signals.size())) {
        std::swap(m.signals[static_cast<std::size_t>(sel_sig_)], m.signals[static_cast<std::size_t>(sel_sig_ + 1)]);
        ++sel_sig_;
        dirty_ = true;
    }

    const float em = ImGui::GetFontSize();
    const float h  = std::min(ImGui::GetContentRegionAvail().y * 0.4f, em * 1.6f * (m.signals.size() + 1.5f));
    if (ImGui::BeginTable("##sigs", 9,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, std::max(h, em * 5.0f)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, em * 3.5f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, em * 3.5f);
        ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, em * 5);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, em * 4);
        ImGui::TableSetupColumn("Factor", ImGuiTableColumnFlags_WidthFixed, em * 5);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, em * 5);
        ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthFixed, em * 5);
        ImGui::TableSetupColumn("Values", ImGuiTableColumnFlags_WidthFixed, em * 4);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < m.signals.size(); ++i) {
            const auto& s = m.signals[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            if (m.sender.empty() && s.receivers.empty())   // belongs to no node
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(170, 40, 40, 110));
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, signal_color(static_cast<int>(i), 1.0f) | 0xFF303030u);
            if (ImGui::Selectable(s.name.c_str(), sel_sig_ == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns))
                sel_sig_ = static_cast<int>(i);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn(); ImGui::Text("%d", s.start_bit);
            ImGui::TableNextColumn(); ImGui::Text("%d", s.bit_size);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.little_endian ? "Intel" : "Motorola");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.float_type == 1 ? "float" : s.float_type == 2 ? "double" : s.is_signed ? "signed" : "unsigned");
            ImGui::TableNextColumn(); ImGui::Text("%g", s.factor);
            ImGui::TableNextColumn(); ImGui::Text("%g", s.offset);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.unit.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%zu", s.values.size());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void DbcEditor::render_signal(DbcMessage& m, DbcSignal& s)
{
    (void)m;
    const float em = ImGui::GetFontSize();
    ImGui::Text("Signal");
    ImGui::Separator();

    ImGui::SetNextItemWidth(em * 16.0f);
    dirty_ |= ImGui::InputText("Name##sig", &s.name);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 7.0f);
    if (ImGui::InputInt("Start bit", &s.start_bit, 1, 8)) {
        s.start_bit = std::max(0, s.start_bit);
        dirty_      = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 7.0f);
    if (ImGui::InputInt("Bits", &s.bit_size, 1, 8)) {
        s.bit_size = std::clamp(s.bit_size, 1, 64);
        dirty_     = true;
    }

    ImGui::SetNextItemWidth(em * 9.0f);
    int order = s.little_endian ? 0 : 1;
    if (ImGui::Combo("Byte order", &order, "Intel (little)\0Motorola (big)\0")) {
        s.little_endian = order == 0;
        dirty_          = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 8.0f);
    int sign = s.float_type == 1 ? 2 : s.float_type == 2 ? 3 : s.is_signed ? 1 : 0;
    if (ImGui::Combo("Type", &sign, "unsigned\0signed\0float (32 bit)\0double (64 bit)\0")) {
        s.float_type = sign == 2 ? 1 : sign == 3 ? 2 : 0;
        s.is_signed  = sign == 1 || sign >= 2;   // floats are written as signed
        if (sign == 2) s.bit_size = 32;
        if (sign == 3) s.bit_size = 64;
        dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 7.0f);
    dirty_ |= ImGui::InputText("Mux", &s.mux);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("empty = plain, M = multiplexer switch, m<N> = active when the switch is N");

    ImGui::SetNextItemWidth(em * 9.0f);
    dirty_ |= ImGui::InputDouble("Factor", &s.factor, 0, 0, "%.9g");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 9.0f);
    dirty_ |= ImGui::InputDouble("Offset", &s.offset, 0, 0, "%.9g");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 9.0f);
    dirty_ |= ImGui::InputDouble("Min", &s.min, 0, 0, "%.9g");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 9.0f);
    dirty_ |= ImGui::InputDouble("Max", &s.max, 0, 0, "%.9g");
    ImGui::SameLine();
    if (ImGui::Button("Auto range")) {
        const double raw_lo = s.float_type == 1 ? -3.4028234663852886e38 : s.float_type == 2 ? -1.7976931348623157e308 : s.is_signed ? -std::pow(2.0, s.bit_size - 1) : 0.0;
        const double raw_hi = s.float_type == 1 ? 3.4028234663852886e38 : s.float_type == 2 ? 1.7976931348623157e308 : s.is_signed ? std::pow(2.0, s.bit_size - 1) - 1 : std::pow(2.0, s.bit_size) - 1;
        s.min = std::min(raw_lo * s.factor + s.offset, raw_hi * s.factor + s.offset);
        s.max = std::max(raw_lo * s.factor + s.offset, raw_hi * s.factor + s.offset);
        dirty_ = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Min / max from the raw range, factor and offset");

    ImGui::SetNextItemWidth(em * 9.0f);
    dirty_ |= ImGui::InputText("Unit", &s.unit);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-em * 6.0f);
    dirty_ |= ImGui::InputText("Comment##sig", &s.comment);

    // receivers
    ImGui::TextUnformatted("Receivers:");
    if (doc_.nodes.empty())
        ImGui::SameLine(), ImGui::TextDisabled("(no nodes)");
    for (std::size_t i = 0; i < doc_.nodes.size(); ++i) {
        const auto& name = doc_.nodes[i].name;
        auto        it   = std::ranges::find(s.receivers, name);
        bool        rx   = it != s.receivers.end();
        ImGui::SameLine();
        if (ImGui::Checkbox(name.c_str(), &rx)) {
            if (rx)
                s.receivers.push_back(name);
            else
                s.receivers.erase(it);
            dirty_ = true;
        }
    }

    // value table
    ImGui::Spacing();
    ImGui::Text("Value table (%zu)", s.values.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("+##val")) {
        int64_t next = 0;
        for (const auto& [v, d] : s.values)
            next = std::max(next, v + 1);
        s.values.emplace_back(next, std::format("State_{}", next));
        dirty_ = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add entry (next free value)");
    ImGui::SameLine();
    if (ImGui::SmallButton("sort##val")) {
        std::ranges::stable_sort(s.values, {}, &std::pair<int64_t, std::string>::first);
        dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("clear##val")) {
        s.values.clear();
        dirty_ = true;
    }

    if (ImGui::BeginTable("##vals", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                          ImVec2(0, std::max(ImGui::GetContentRegionAvail().y, em * 4.0f)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, em * 8);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, em * 2);
        ImGui::TableHeadersRow();
        int remove = -1;
        for (std::size_t i = 0; i < s.values.size(); ++i) {
            auto& [v, d] = s.values[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            dirty_ |= ImGui::InputScalar("##v", ImGuiDataType_S64, &v);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            dirty_ |= ImGui::InputText("##d", &d);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x"))
                remove = static_cast<int>(i);
            ImGui::PopID();
        }
        if (remove >= 0) {
            s.values.erase(s.values.begin() + remove);
            dirty_ = true;
        }
        ImGui::EndTable();
    }
}

void DbcEditor::render_dialogs()
{
    if (browse_ != Browse::None) {
        pending_ = browse_;
        browse_  = Browse::None;
        if (pending_ == Browse::Import)
            browser_.open("Import DBC", FileBrowser::Mode::Open, {".dbc"}, path_);
        else
            browser_.open("Save DBC", FileBrowser::Mode::Save, {".dbc"}, path_);
    }
    if (auto p = browser_.render()) {
        if (pending_ == Browse::Import) {
            load_path(p->string());
        } else if (pending_ == Browse::SaveAs) {
            auto path = *p;
            if (path.extension().empty())
                path += ".dbc";
            save_to(path.string());
        }
        pending_ = Browse::None;
    }

    if (ask_discard_) {
        ImGui::OpenPopup("Unsaved changes");
        ask_discard_ = false;
    }
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("The current DBC has unsaved changes.");
        if (ImGui::Button("Discard")) {
            dirty_ = false;
            if (!pending_open_.empty())
                load_path(std::exchange(pending_open_, {}));
            else if (after_discard_ == Browse::New)
                new_document();
            else
                browse_ = after_discard_;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pending_open_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
