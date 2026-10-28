#include "DbcNetwork.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <imgui_stdlib.h>
#include <set>

namespace {

ImVec2 operator+(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
ImVec2 operator-(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }
ImVec2 operator*(ImVec2 a, float s) { return {a.x * s, a.y * s}; }

ImU32 msg_color(std::size_t idx, int alpha = 255)
{
    const ImColor c = ImColor::HSV(std::fmod(static_cast<float>(idx) * 0.137f + 0.05f, 1.0f), 0.6f, 0.85f);
    return IM_COL32(static_cast<int>(c.Value.x * 255), static_cast<int>(c.Value.y * 255), static_cast<int>(c.Value.z * 255), alpha);
}

void add_default_signal(DbcMessage& m)
{
    DbcSignal s;
    s.name = "Signal_1";
    s.max  = 255;
    m.signals.push_back(std::move(s));
}

void add_receiver(DbcMessage& m, const std::string& node)
{
    if (m.signals.empty())
        add_default_signal(m);
    for (auto& s : m.signals)
        if (std::ranges::find(s.receivers, node) == s.receivers.end())
            s.receivers.push_back(node);
}

void remove_receiver(DbcMessage& m, const std::string& node)
{
    for (auto& s : m.signals)
        std::erase(s.receivers, node);
}

uint32_t next_id(const DbcDoc& doc)
{
    uint32_t next = 0x100;
    for (const auto& o : doc.messages)
        next = std::max(next, o.id + 1);
    return next;
}

DbcMessage new_message(const DbcDoc& doc, const std::string& from, const std::string& to)
{
    DbcMessage m;
    m.id     = next_id(doc);
    m.name   = std::format("{}_to_{}_{}", from, to, doc.messages.size() + 1);
    m.sender = from;
    add_receiver(m, to);
    return m;
}

enum Side { Left, Right, Top, Bottom };
constexpr ImVec2 NORMAL[4] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

struct Rect {
    ImVec2 a, b;
    ImVec2 center() const { return {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f}; }
};

ImVec2 bezier(ImVec2 p0, ImVec2 c0, ImVec2 c1, ImVec2 p1, float t)
{
    const float u = 1.0f - t;
    return p0 * (u * u * u) + c0 * (3 * u * u * t) + c1 * (3 * u * t * t) + p1 * (t * t * t);
}

float dist_to_segment(ImVec2 p, ImVec2 a, ImVec2 b)
{
    const ImVec2 ab = b - a, ap = p - a;
    const float  l2 = ab.x * ab.x + ab.y * ab.y;
    const float  t  = l2 > 0 ? std::clamp((ap.x * ab.x + ap.y * ab.y) / l2, 0.0f, 1.0f) : 0.0f;
    const ImVec2 d  = ap - ab * t;
    return std::sqrt(d.x * d.x + d.y * d.y);
}

// Nearest point on the border of r for a point p.
ImVec2 border_point(const Rect& r, ImVec2 p)
{
    const float x = std::clamp(p.x, r.a.x, r.b.x), y = std::clamp(p.y, r.a.y, r.b.y);
    if (x > r.a.x && x < r.b.x && y > r.a.y && y < r.b.y) {
        const float dl = x - r.a.x, dr = r.b.x - x, dt = y - r.a.y, db = r.b.y - y;
        const float m  = std::min({dl, dr, dt, db});
        if (m == dl) return {r.a.x, y};
        if (m == dr) return {r.b.x, y};
        if (m == dt) return {x, r.a.y};
        return {x, r.b.y};
    }
    return {x, y};
}

} // namespace

std::ostream& operator<<(std::ostream& os, const DbcNetwork& n)
{
    return os << "DbcNetwork(" << n.boxes_.size() << " nodes)";
}

DbcNetwork::Result DbcNetwork::render(DbcDoc& doc, int& sel_msg, int& sel_node)
{
    Result      res;
    const float em = ImGui::GetFontSize();
    const ImVec2 default_size(em * 11.0f, em * 4.2f);
    const float strip_h = em * 8.0f;
    ImGuiIO&    io = ImGui::GetIO();

    // boxes for new nodes: on a circle
    for (std::size_t i = 0; i < doc.nodes.size(); ++i) {
        auto& bx = boxes_[doc.nodes[i].name];
        if (bx.size.x <= 0) {
            const float cnt = static_cast<float>(std::max<std::size_t>(doc.nodes.size(), 1));
            const float a   = 6.2831853f * static_cast<float>(i) / cnt;
            const float r   = em * (6.0f + 2.0f * cnt);
            if (bx.pos.x == 0 && bx.pos.y == 0)
                bx.pos = ImVec2(em * 20 + r * std::cos(a), em * 12 + r * std::sin(a));
            bx.size = default_size;
        }
    }

    // links derived from the document: sender -> every receiver of any signal
    struct Link {
        std::size_t msg;
        std::string from, to;
    };
    std::vector<Link> links;
    for (std::size_t mi = 0; mi < doc.messages.size(); ++mi) {
        const auto& m = doc.messages[mi];
        if (m.sender.empty() || doc.find_node(m.sender) < 0)
            continue;
        std::set<std::string> rx;
        for (const auto& s : m.signals)
            rx.insert(s.receivers.begin(), s.receivers.end());
        for (const auto& n : doc.nodes)
            if (rx.contains(n.name) && n.name != m.sender)
                links.push_back({mi, m.sender, n.name});
    }
    int sel_link = -1;
    for (std::size_t i = 0; i < links.size(); ++i)
        if (static_cast<int>(links[i].msg) == sel_link_msg_ && links[i].to == sel_link_to_)
            sel_link = static_cast<int>(i);
    if (sel_link < 0)
        sel_link_msg_ = -1;

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##netcanvas", ImVec2(0, std::max(avail.y - strip_h, em * 8.0f)), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 c0 = ImGui::GetCursorScreenPos(), csz = ImGui::GetContentRegionAvail();
    const ImVec2 c1(c0.x + csz.x, c0.y + csz.y);
    // no item here: hit testing is done by hand, so the canvas must not depend on item hover / activation
    ImGui::Dummy(csz);
    const bool   hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(c0, c1) && !ImGui::IsAnyItemHovered();

    {
        const ImVec2 keep = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(c1.x - em * 9.0f, c0.y + 4));
        if (ImGui::SmallButton("Fit"))
            fit_ = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f%%", zoom_ * 100.0f);
        ImGui::SetCursorScreenPos(keep);
    }
    // zoom with the wheel around the mouse; "Fit" shows all boxes
    if (hovered && io.MouseWheel != 0.0f) {
        const float nz = std::clamp(zoom_ * std::pow(1.1f, io.MouseWheel), 0.15f, 3.0f);
        const ImVec2 g = (io.MousePos - c0 - pan_) * (1.0f / zoom_);   // grid point under the mouse
        zoom_          = nz;
        pan_           = io.MousePos - c0 - g * zoom_;
    }
    if (fit_ && !boxes_.empty()) {
        ImVec2 lo(1e9f, 1e9f), hi(-1e9f, -1e9f);
        for (const auto& n : doc.nodes) {
            const Box& bx = boxes_[n.name];
            lo = ImVec2(std::min(lo.x, bx.pos.x), std::min(lo.y, bx.pos.y));
            hi = ImVec2(std::max(hi.x, bx.pos.x + bx.size.x), std::max(hi.y, bx.pos.y + bx.size.y));
        }
        if (hi.x > lo.x && hi.y > lo.y) {
            zoom_ = std::clamp(std::min(csz.x / ((hi.x - lo.x) * 1.15f), csz.y / ((hi.y - lo.y) * 1.15f)), 0.15f, 1.5f);
            pan_  = ImVec2((csz.x - (hi.x - lo.x) * zoom_) * 0.5f - lo.x * zoom_, (csz.y - (hi.y - lo.y) * zoom_) * 0.5f - lo.y * zoom_);
        }
    }
    fit_ = false;
    const float  z   = zoom_;
    const float  ez  = em * z;   // scaled font unit
    const ImVec2 org = c0 + pan_;
    const float  fs  = ImGui::GetFontSize() * z;
    const auto   text_size = [&](const std::string& t) { return ImGui::CalcTextSize(t.c_str()) * z; };
    const ImVec2 mp      = io.MousePos;

    const auto rect_of = [&](const std::string& n) {
        const Box& bx = boxes_[n];
        const ImVec2 a = org + bx.pos * z;
        return Rect{a, a + bx.size * z};
    };

    // ---- link geometry: dock on the side facing the other node, spread along the side
    struct Geo {
        int    sa = Right, sb = Left;
        ImVec2 p0{}, p1{}, k0{}, k1{};
    };
    std::vector<Geo> geo(links.size());
    std::map<std::pair<std::string, int>, std::vector<std::pair<int, bool>>> groups;
    for (std::size_t i = 0; i < links.size(); ++i) {
        const Rect ra = rect_of(links[i].from), rb = rect_of(links[i].to);
        const ImVec2 ca = ra.center(), cb = rb.center();
        const float  dx = cb.x - ca.x, dy = cb.y - ca.y;
        const float  wsum = (ra.b.x - ra.a.x + rb.b.x - rb.a.x) * 0.5f, hsum = (ra.b.y - ra.a.y + rb.b.y - rb.a.y) * 0.5f;
        if (std::fabs(dx) * hsum >= std::fabs(dy) * wsum) {
            geo[i].sa = dx >= 0 ? Right : Left;
            geo[i].sb = dx >= 0 ? Left : Right;
        } else {
            geo[i].sa = dy >= 0 ? Bottom : Top;
            geo[i].sb = dy >= 0 ? Top : Bottom;
        }
        groups[{links[i].from, geo[i].sa}].push_back({static_cast<int>(i), true});
        groups[{links[i].to, geo[i].sb}].push_back({static_cast<int>(i), false});
    }
    for (auto& [key, list] : groups) {
        const Rect r     = rect_of(key.first);
        const bool vert  = key.second == Left || key.second == Right;   // side runs along y
        const auto other = [&](const std::pair<int, bool>& e) {
            const Link& l = links[static_cast<std::size_t>(e.first)];
            const ImVec2 c = rect_of(e.second ? l.to : l.from).center();
            return vert ? c.y : c.x;
        };
        std::ranges::sort(list, [&](const auto& x, const auto& y) { return other(x) < other(y); });
        for (std::size_t k = 0; k < list.size(); ++k) {
            const float t = static_cast<float>(k + 1) / static_cast<float>(list.size() + 1);
            ImVec2      p;
            switch (key.second) {
                case Left:   p = {r.a.x, r.a.y + (r.b.y - r.a.y) * t}; break;
                case Right:  p = {r.b.x, r.a.y + (r.b.y - r.a.y) * t}; break;
                case Top:    p = {r.a.x + (r.b.x - r.a.x) * t, r.a.y}; break;
                default:     p = {r.a.x + (r.b.x - r.a.x) * t, r.b.y}; break;
            }
            (list[k].second ? geo[static_cast<std::size_t>(list[k].first)].p0 : geo[static_cast<std::size_t>(list[k].first)].p1) = p;
        }
    }
    for (auto& g : geo) {
        const float dx = g.p1.x - g.p0.x, dy = g.p1.y - g.p0.y;
        const float d  = std::max(ez * 3.0f, std::sqrt(dx * dx + dy * dy) * 0.35f);
        g.k0           = g.p0 + NORMAL[g.sa] * d;
        g.k1           = g.p1 + NORMAL[g.sb] * d;
    }
    const auto link_at = [&](ImVec2 p) {
        for (std::size_t i = 0; i < links.size(); ++i) {
            ImVec2 prev = geo[i].p0;
            for (int s = 1; s <= 24; ++s) {
                const ImVec2 cur = bezier(geo[i].p0, geo[i].k0, geo[i].k1, geo[i].p1, static_cast<float>(s) / 24.0f);
                if (dist_to_segment(p, prev, cur) < 6.0f)
                    return static_cast<int>(i);
                prev = cur;
            }
        }
        return -1;
    };

    // ---- half-connected messages: an output nobody receives / an input without sender -> dashed stub
    struct Stub {
        std::size_t msg;
        bool        out;
        ImVec2      p0, p1;   // p0 on the box border, p1 free end
    };
    std::vector<Stub> stubs;
    for (const auto& n : doc.nodes) {
        const Rect r = rect_of(n.name);
        std::vector<std::pair<std::size_t, bool>> list;   // (message, out)
        for (std::size_t mi = 0; mi < doc.messages.size(); ++mi) {
            const auto& m = doc.messages[mi];
            const bool has_rx = std::ranges::any_of(m.signals, [](const DbcSignal& sg) { return !sg.receivers.empty(); });
            if (m.sender == n.name && !has_rx)
                list.push_back({mi, true});
            else if (m.sender.empty() && std::ranges::any_of(m.signals, [&](const DbcSignal& sg) {
                         return std::ranges::find(sg.receivers, n.name) != sg.receivers.end();
                     }))
                list.push_back({mi, false});
        }
        for (const bool out : {true, false}) {
            std::vector<std::size_t> msgs;
            for (const auto& e : list)
                if (e.second == out)
                    msgs.push_back(e.first);
            for (std::size_t k = 0; k < msgs.size(); ++k) {
                const float  t = static_cast<float>(k + 1) / static_cast<float>(msgs.size() + 1);
                const float  y = r.a.y + (r.b.y - r.a.y) * t;
                const ImVec2 p0(out ? r.b.x : r.a.x, y);
                stubs.push_back({msgs[k], out, p0, ImVec2(p0.x + (out ? 1.0f : -1.0f) * ez * 4.0f, y)});
            }
        }
    }
    const auto stub_at = [&](ImVec2 p) {
        for (std::size_t i = 0; i < stubs.size(); ++i)
            if (dist_to_segment(p, stubs[i].p0, stubs[i].p1) < 6.0f)
                return static_cast<int>(i);
        return -1;
    };

    // ---- hit testing of the boxes
    enum class Region { None, Body, Border, Resize };
    struct Hit {
        std::string name;
        Region      region = Region::None;
    };
    const auto hit = [&](ImVec2 p) {
        for (std::size_t i = doc.nodes.size(); i-- > 0;) {
            const std::string& n = doc.nodes[i].name;
            const Rect         r = rect_of(n);
            if (p.x < r.a.x - 5 || p.x > r.b.x + 5 || p.y < r.a.y - 5 || p.y > r.b.y + 5)
                continue;
            if (p.x > r.b.x - 12 && p.y > r.b.y - 12 && p.x <= r.b.x + 2 && p.y <= r.b.y + 2)
                return Hit{n, Region::Resize};
            const bool inner = p.x > r.a.x + 7 && p.x < r.b.x - 7 && p.y > r.a.y + 7 && p.y < r.b.y - 7;
            return Hit{n, inner ? Region::Body : Region::Border};
        }
        return Hit{};
    };

    // ---- input
    const Hit under = hit(mp);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (under.region != Region::None) {
            sel_node_name_ = under.name;
            sel_node       = doc.find_node(under.name);
            sel_link_msg_  = -1;
            drag_node_     = under.name;
            drag_          = under.region == Region::Resize ? Drag::Resize : under.region == Region::Border ? Drag::Link : Drag::Move;
            if (under.region == Region::Body && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                open_node_ = under.name;
                dlg_out_ = dlg_in_ = dlg_pool_ = -1;
                drag_ = Drag::None;
            }
        } else if (const int li = link_at(mp); li >= 0) {
            sel_link_msg_  = static_cast<int>(links[static_cast<std::size_t>(li)].msg);
            sel_link_to_   = links[static_cast<std::size_t>(li)].to;
            sel_msg        = sel_link_msg_;
            sel_node_name_.clear();
        } else if (const int si = stub_at(mp); si >= 0) {
            sel_msg       = static_cast<int>(stubs[static_cast<std::size_t>(si)].msg);
            sel_link_msg_ = -1;
            sel_node_name_.clear();
        } else {
            sel_link_msg_ = -1;
            sel_node_name_.clear();
            drag_ = Drag::Pan;
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
        drag_ = Drag::Pan;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (under.region == Region::None) {
            if (const int li = link_at(mp); li >= 0) {
                ctx_from_ = links[static_cast<std::size_t>(li)].from;
                ctx_to_   = links[static_cast<std::size_t>(li)].to;
                ImGui::OpenPopup("##linkctx");
            } else {
                ctx_pos_ = (mp - org) * (1.0f / z);
                ImGui::OpenPopup("##netctx");
            }
        }
    }
    if (drag_ != Drag::None) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            if (drag_ == Drag::Link && under.region != Region::None && under.name != drag_node_) {
                pending_from_ = drag_node_;
                pending_to_   = under.name;
                ImGui::OpenPopup("##newlink");
            }
            drag_ = Drag::None;
        } else if (drag_ == Drag::Pan) {
            pan_ = pan_ + io.MouseDelta;
        } else if (drag_ == Drag::Move && boxes_.contains(drag_node_)) {
            boxes_[drag_node_].pos = boxes_[drag_node_].pos + io.MouseDelta * (1.0f / z);
        } else if (drag_ == Drag::Resize && boxes_.contains(drag_node_)) {
            Box& bx = boxes_[drag_node_];
            bx.size = ImVec2(std::max(em * 6.0f, bx.size.x + io.MouseDelta.x / z), std::max(em * 3.0f, bx.size.y + io.MouseDelta.y / z));
        }
    }

    // ---- drawing
    dl->PushClipRect(c0, c1, true);
    // grid
    {
        const float step = std::max(ez * 2.5f, 6.0f);
        const ImU32 gc   = ImGui::GetColorU32(ImGuiCol_Separator, 0.25f);
        for (float x = c0.x + std::fmod(pan_.x, step); x < c1.x; x += step)
            dl->AddLine(ImVec2(x, c0.y), ImVec2(x, c1.y), gc);
        for (float y = c0.y + std::fmod(pan_.y, step); y < c1.y; y += step)
            dl->AddLine(ImVec2(c0.x, y), ImVec2(c1.x, y), gc);
    }

    // links with arrow heads at the receiver
    for (std::size_t i = 0; i < links.size(); ++i) {
        const auto& l   = links[i];
        const auto& g   = geo[i];
        const bool  sel = static_cast<int>(i) == sel_link;
        const ImU32 col = msg_color(l.msg, sel ? 255 : 200);
        dl->AddBezierCubic(g.p0, g.k0, g.k1, g.p1, col, sel ? 4.0f : 2.5f);
        dl->AddCircleFilled(g.p0, 4.0f, col);
        const ImVec2 n = NORMAL[g.sb], t(-n.y, n.x);   // n points away from the receiver
        const float  s = ez * 0.55f;
        dl->AddTriangleFilled(g.p1, g.p1 + n * (s * 1.6f) + t * s, g.p1 + n * (s * 1.6f) - t * s, col);
    }

    // stubs of half-connected messages
    for (const auto& st : stubs) {
        const auto& m   = doc.messages[st.msg];
        const bool  sel = static_cast<int>(st.msg) == sel_msg;
        const ImU32 col = msg_color(st.msg, sel ? 255 : 170);
        const float len = st.p1.x - st.p0.x, dir = len >= 0 ? 1.0f : -1.0f;
        for (float x = 0; x < std::fabs(len); x += ez * 0.8f)   // dashed
            dl->AddLine(ImVec2(st.p0.x + dir * x, st.p0.y), ImVec2(st.p0.x + dir * std::min(x + ez * 0.45f, std::fabs(len)), st.p0.y),
                        col, sel ? 3.5f : 2.0f);
        const ImVec2 tip = st.out ? st.p1 : st.p0;                    // arrow: out = away from the node, in = into the node
        const float  ad  = st.out ? dir : -dir;
        const float  s2  = ez * 0.45f;
        dl->AddTriangleFilled(tip, ImVec2(tip.x - ad * s2 * 1.6f, tip.y - s2), ImVec2(tip.x - ad * s2 * 1.6f, tip.y + s2), col);
        const auto   text = std::format("{} 0x{:X}", m.name, m.id);
        const ImVec2 sz   = text_size(text);
        const ImVec2 tp(st.out ? st.p1.x + 4 : st.p1.x - sz.x - 4, st.p1.y - sz.y * 0.5f);
        if (z > 0.3f)
            dl->AddText(ImGui::GetFont(), fs, tp, col, text.c_str());
    }

    // boxes
    for (const auto& n : doc.nodes) {
        const Rect  r   = rect_of(n.name);
        const bool  sel = n.name == sel_node_name_;
        int tx = 0, rx = 0;
        for (const auto& m : doc.messages) {
            tx += m.sender == n.name;
            rx += std::ranges::any_of(m.signals, [&](const DbcSignal& s) {
                return std::ranges::find(s.receivers, n.name) != s.receivers.end();
            });
        }
        dl->AddRectFilled(r.a, r.b, ImGui::GetColorU32(ImGuiCol_FrameBg), 6.0f);
        dl->AddRectFilled(r.a, ImVec2(r.b.x, r.a.y + ez * 1.7f), ImGui::GetColorU32(ImGuiCol_TitleBgActive), 6.0f,
                          ImDrawFlags_RoundCornersTop);
        dl->AddRect(r.a, r.b, sel ? IM_COL32(255, 200, 60, 255) : ImGui::GetColorU32(ImGuiCol_Border), 6.0f, 0, sel ? 2.5f : 1.0f);
        dl->PushClipRect(r.a, r.b, true);
        dl->AddText(ImGui::GetFont(), fs, ImVec2(r.a.x + ez * 0.5f, r.a.y + ez * 0.3f), ImGui::GetColorU32(ImGuiCol_Text), n.name.c_str());
        if (z > 0.3f)
            dl->AddText(ImGui::GetFont(), fs, ImVec2(r.a.x + ez * 0.5f, r.a.y + ez * 2.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                        std::format("sends {}   receives {}", tx, rx).c_str());
        dl->PopClipRect();
        dl->AddTriangleFilled(r.b, ImVec2(r.b.x - 11, r.b.y), ImVec2(r.b.x, r.b.y - 11), ImGui::GetColorU32(ImGuiCol_Text, 0.5f));
    }

    // hover hint on the border (= start of a link)
    if (hovered && drag_ == Drag::None && under.region == Region::Border)
        dl->AddCircleFilled(border_point(rect_of(under.name), mp), 5.0f, IM_COL32(255, 255, 255, 220));
    // rubber band while linking
    if (drag_ == Drag::Link) {
        const ImVec2 s = border_point(rect_of(drag_node_), mp);
        dl->AddLine(s, mp, IM_COL32(255, 255, 255, 220), 2.0f);
        if (under.region != Region::None && under.name != drag_node_) {
            const Rect r = rect_of(under.name);
            dl->AddRect(r.a, r.b, IM_COL32(120, 255, 120, 255), 6.0f, 0, 3.0f);
        }
    }

    // labels in the middle of the links
    for (std::size_t i = 0; i < links.size(); ++i) {
        const auto&  m    = doc.messages[links[i].msg];
        const auto   text = std::format("{} 0x{:X}", m.name, m.id);
        const ImVec2 sz   = text_size(text);
        const ImVec2 mid  = bezier(geo[i].p0, geo[i].k0, geo[i].k1, geo[i].p1, 0.5f);
        const ImVec2 p(mid.x - sz.x * 0.5f, mid.y - sz.y * 0.5f);
        dl->AddRectFilled(ImVec2(p.x - 3, p.y - 1), ImVec2(p.x + sz.x + 3, p.y + sz.y + 1), ImGui::GetColorU32(ImGuiCol_WindowBg, 0.85f), 3.0f);
        if (z > 0.3f)
            dl->AddText(ImGui::GetFont(), fs, p, msg_color(links[i].msg), text.c_str());
    }
    dl->PopClipRect();

    // ---- popups
    if (ImGui::BeginPopup("##newlink")) {
        ImGui::Text("%s -> %s", pending_from_.c_str(), pending_to_.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("New message")) {
            doc.messages.push_back(new_message(doc, pending_from_, pending_to_));
            sel_msg     = static_cast<int>(doc.messages.size()) - 1;
            res.changed = true;
        }
        bool header = false;
        for (std::size_t i = 0; i < doc.messages.size(); ++i) {
            auto& m = doc.messages[i];
            if (!m.sender.empty() && m.sender != pending_from_)
                continue;
            const bool already = !m.signals.empty() && std::ranges::all_of(m.signals, [&](const DbcSignal& s) {
                return std::ranges::find(s.receivers, pending_to_) != s.receivers.end();
            });
            if (already && m.sender == pending_from_)
                continue;
            if (!header) {
                ImGui::SeparatorText("Existing message (also to this node)");
                header = true;
            }
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::MenuItem(m.name.c_str())) {
                m.sender = pending_from_;
                add_receiver(m, pending_to_);
                sel_msg     = static_cast<int>(i);
                res.changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##linkctx")) {
        ImGui::Text("%s -> %s", ctx_from_.c_str(), ctx_to_.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Add message")) {
            doc.messages.push_back(new_message(doc, ctx_from_, ctx_to_));
            sel_msg     = static_cast<int>(doc.messages.size()) - 1;
            res.changed = true;
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##netctx")) {
        if (ImGui::MenuItem("Add node")) {
            std::string name;
            for (std::size_t n = doc.nodes.size() + 1;; ++n) {
                name = std::format("Node_{}", n);
                if (doc.find_node(name) < 0)
                    break;
            }
            doc.nodes.push_back({name, {}});
            boxes_[name] = Box{ctx_pos_, default_size};
            res.changed  = true;
        }
        ImGui::EndPopup();
    }

    // Delete: selected link (= receiver of that message) or node
    if (hovered && ImGui::IsKeyPressed(ImGuiKey_Delete) && !io.WantTextInput) {
        if (sel_link >= 0) {
            const auto& l = links[static_cast<std::size_t>(sel_link)];
            remove_receiver(doc.messages[l.msg], l.to);
            sel_link_msg_ = -1;
            res.changed   = true;
        } else if (const int ni = doc.find_node(sel_node_name_); ni >= 0) {
            boxes_.erase(sel_node_name_);
            doc.remove_node(static_cast<std::size_t>(ni));
            sel_node_name_.clear();
            sel_node    = -1;
            res.changed = true;
        }
    }
    ImGui::EndChild();

    // ---- detail strip of the selected message
    if (sel_msg >= 0 && sel_msg < static_cast<int>(doc.messages.size())) {
        auto&       m = doc.messages[static_cast<std::size_t>(sel_msg)];
        std::string rx;
        std::set<std::string> seen;
        for (const auto& s : m.signals)
            for (const auto& r : s.receivers)
                if (seen.insert(r).second)
                    rx += (rx.empty() ? "" : ", ") + r;
        ImGui::SetNextItemWidth(em * 14.0f);
        res.changed |= ImGui::InputText("Name", &m.name);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 7.0f);
        res.changed |= ImGui::InputScalar("ID (hex)", ImGuiDataType_U32, &m.id, nullptr, nullptr, "%X",
                                          ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        res.changed |= ImGui::Checkbox("Ext", &m.extended);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 6.0f);
        if (ImGui::InputInt("Bytes", &m.size)) {
            m.size      = std::clamp(m.size, 1, 64);
            res.changed = true;
        }
        ImGui::Text("%s  ->  %s", m.sender.empty() ? "(no sender)" : m.sender.c_str(), rx.empty() ? "(no receiver)" : rx.c_str());
        if (sel_link >= 0) {
            const Link& l = links[static_cast<std::size_t>(sel_link)];
            ImGui::SameLine();
            if (ImGui::Button(std::format("+ Message {} -> {}", l.from, l.to).c_str())) {
                doc.messages.push_back(new_message(doc, l.from, l.to));
                sel_msg     = static_cast<int>(doc.messages.size()) - 1;
                res.changed = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Open in table"))
            res.open_table = true;
        ImGui::SameLine();
        if (ImGui::Button("Delete message")) {
            doc.messages.erase(doc.messages.begin() + sel_msg);
            sel_msg       = -1;
            sel_link_msg_ = -1;
            res.changed   = true;
        } else {
            ImGui::TextUnformatted("Receivers:");
            for (const auto& n : doc.nodes) {
                if (n.name == m.sender)
                    continue;
                bool on = seen.contains(n.name);
                ImGui::SameLine();
                if (ImGui::Checkbox(n.name.c_str(), &on)) {
                    if (on)
                        add_receiver(m, n.name);
                    else
                        remove_receiver(m, n.name);
                    res.changed = true;
                }
            }
        }
    } else {
        ImGui::TextDisabled("Double-click a node: in / out.  Drag from a node's border onto another node: message.  "
                            "Drag the corner: resize.  Right-click: add node / message.  Del: remove selection.  "
                            "Drag the background (or middle mouse): pan.  Wheel: zoom.");
    }
    render_node_dialog(doc, res);
    return res;
}

void DbcNetwork::render_node_dialog(DbcDoc& doc, Result& res)
{
    if (open_node_.empty())
        return;
    if (doc.find_node(open_node_) < 0) {
        open_node_.clear();
        return;
    }
    const float em   = ImGui::GetFontSize();
    bool        open = true;
    ImGui::SetNextWindowSize(ImVec2(em * 56, em * 28), ImGuiCond_Appearing);
    if (ImGui::Begin(std::format("Node {}###nodedlg", open_node_).c_str(), &open)) {
        const std::string node = open_node_;
        const auto sends = [&](const DbcMessage& m) { return m.sender == node; };
        const auto recv  = [&](const DbcMessage& m) {
            return std::ranges::any_of(m.signals, [&](const DbcSignal& s) {
                return std::ranges::find(s.receivers, node) != s.receivers.end();
            });
        };
        const auto label = [](const DbcMessage& m) { return std::format("0x{:X}  {}", m.id, m.name); };

        if (ImGui::BeginTable("##dlg", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("node", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("mid", ImGuiTableColumnFlags_WidthFixed, em * 8.5f);
            ImGui::TableSetupColumn("pool", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableNextRow();

            // left: what this node sends / receives
            ImGui::TableNextColumn();
            const float half = (ImGui::GetContentRegionAvail().y - em * 3.0f) * 0.5f;
            ImGui::TextUnformatted("Outputs (sends)");
            if (ImGui::BeginChild("##outs", ImVec2(0, half), ImGuiChildFlags_Border)) {
                for (std::size_t i = 0; i < doc.messages.size(); ++i)
                    if (sends(doc.messages[i])) {
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Selectable(label(doc.messages[i]).c_str(), dlg_out_ == static_cast<int>(i))) {
                            dlg_out_ = static_cast<int>(i);
                            dlg_in_  = -1;
                        }
                        ImGui::PopID();
                    }
            }
            ImGui::EndChild();
            ImGui::TextUnformatted("Inputs (receives)");
            if (ImGui::BeginChild("##ins", ImVec2(0, half), ImGuiChildFlags_Border)) {
                for (std::size_t i = 0; i < doc.messages.size(); ++i)
                    if (recv(doc.messages[i])) {
                        ImGui::PushID(static_cast<int>(i));
                        const auto& m = doc.messages[i];
                        const auto  t = label(m) + (m.sender.empty() ? "" : "   <- " + m.sender);
                        if (ImGui::Selectable(t.c_str(), dlg_in_ == static_cast<int>(i))) {
                            dlg_in_  = static_cast<int>(i);
                            dlg_out_ = -1;
                        }
                        ImGui::PopID();
                    }
            }
            ImGui::EndChild();

            // middle: move buttons
            ImGui::TableNextColumn();
            ImGui::Dummy(ImVec2(0, em * 3.0f));
            const bool pool_ok = dlg_pool_ >= 0 && dlg_pool_ < static_cast<int>(doc.messages.size());
            const bool has_sender = pool_ok && !doc.messages[static_cast<std::size_t>(dlg_pool_)].sender.empty();
            ImGui::BeginDisabled(!pool_ok || has_sender);
            if (ImGui::Button("<- Output", ImVec2(-FLT_MIN, 0))) {
                doc.messages[static_cast<std::size_t>(dlg_pool_)].sender = node;
                dlg_pool_   = -1;
                res.changed = true;
            }
            ImGui::EndDisabled();
            if (has_sender && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Already sent by %s - remove it there first", doc.messages[static_cast<std::size_t>(dlg_pool_)].sender.c_str());
            ImGui::BeginDisabled(!pool_ok);
            if (ImGui::Button("<- Input", ImVec2(-FLT_MIN, 0))) {
                add_receiver(doc.messages[static_cast<std::size_t>(dlg_pool_)], node);
                dlg_pool_   = -1;
                res.changed = true;
            }
            ImGui::EndDisabled();
            ImGui::Spacing();
            const bool rm_out = dlg_out_ >= 0 && dlg_out_ < static_cast<int>(doc.messages.size());
            const bool rm_in  = dlg_in_ >= 0 && dlg_in_ < static_cast<int>(doc.messages.size());
            ImGui::BeginDisabled(!rm_out && !rm_in);
            if (ImGui::Button("Remove ->", ImVec2(-FLT_MIN, 0))) {
                if (rm_out) {
                    doc.messages[static_cast<std::size_t>(dlg_out_)].sender.clear();
                } else {
                    for (auto& sg : doc.messages[static_cast<std::size_t>(dlg_in_)].signals)
                        std::erase(sg.receivers, node);
                }
                dlg_out_ = dlg_in_ = -1;
                res.changed = true;
            }
            ImGui::EndDisabled();

            // right: pool of messages without a sender
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Pool (all other messages)");
            ImGui::SameLine();
            if (ImGui::SmallButton("+ new")) {
                DbcMessage m;
                m.id   = next_id(doc);
                m.name = std::format("NewMessage_{}", doc.messages.size() + 1);
                doc.messages.push_back(std::move(m));
                dlg_pool_   = static_cast<int>(doc.messages.size()) - 1;
                res.changed = true;
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##poolf", "filter", &pool_filter_);
            if (ImGui::BeginChild("##pool", ImVec2(0, 0), ImGuiChildFlags_Border)) {
                for (std::size_t i = 0; i < doc.messages.size(); ++i) {
                    const auto& m = doc.messages[i];
                    if (sends(m) || recv(m))
                        continue;   // already in this node's lists
                    const auto t = label(m);
                    if (!pool_filter_.empty() && t.find(pool_filter_) == std::string::npos)
                        continue;
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable((t + (m.sender.empty() ? "" : "   <- " + m.sender)).c_str(), dlg_pool_ == static_cast<int>(i)))
                        dlg_pool_ = static_cast<int>(i);
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::EndTable();
        }
    }
    ImGui::End();
    if (!open)
        open_node_.clear();
}

void DbcNetwork::reset()
{
    boxes_.clear();
    pan_ = ImVec2(0, 0);
    zoom_ = 1.0f;
    drag_ = Drag::None;
    sel_node_name_.clear();
    sel_link_msg_ = -1;
    open_node_.clear();
}

void DbcNetwork::load(const std::string& dbc_path)
{
    reset();
    std::ifstream f(dbc_path + ".net");
    for (std::string line; std::getline(f, line);) {
        std::istringstream in(line);
        std::string        name;
        float              v[4] = {0, 0, 0, 0};
        int                n    = 0;
        in >> name;
        while (n < 4 && in >> v[n])
            ++n;
        if (name.empty() || n < 2)
            continue;
        boxes_[name] = Box{ImVec2(v[0], v[1]), n >= 4 ? ImVec2(v[2], v[3]) : ImVec2(0, 0)};
    }
}

void DbcNetwork::save(const std::string& dbc_path, const DbcDoc& doc) const
{
    std::ofstream f(dbc_path + ".net", std::ios::trunc);
    for (const auto& n : doc.nodes)
        if (auto it = boxes_.find(n.name); it != boxes_.end())
            f << n.name << ' ' << it->second.pos.x << ' ' << it->second.pos.y << ' ' << it->second.size.x << ' '
              << it->second.size.y << '\n';
}
