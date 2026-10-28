#pragma once

#include "dbc/DbcModel.hpp"

#include <format>
#include <imgui.h>
#include <map>
#include <ostream>
#include <sstream>
#include <string>

// Node graph of a DBC (own canvas): boxes = nodes (ECUs), arrows = messages from the sender to every receiver.
// Drag from the border of A to any point of B to create a message A -> B (or extend an existing one: a message
// can go to several nodes). Links dock on the side that faces the other node. Boxes can be moved and resized.
// Everything is derived from the DbcDoc (sender + signal receivers); only the boxes live here / in "<dbc>.net".
class DbcNetwork {
public:
    struct Result {
        bool changed    = false;   // doc was modified
        bool open_table = false;   // "Open in table" clicked
    };

    // sel_msg / sel_node: selection shared with the table view.
    Result render(DbcDoc& doc, int& sel_msg, int& sel_node);

    // Box positions / sizes live in "<dbc>.net" next to the DBC file.
    void load(const std::string& dbc_path);
    void save(const std::string& dbc_path, const DbcDoc& doc) const;
    void reset();

    friend std::ostream& operator<<(std::ostream& os, const DbcNetwork& n);

private:
    struct Box {
        ImVec2 pos{};    // grid space
        ImVec2 size{};   // 0 = default
    };
    enum class Drag { None, Pan, Move, Resize, Link };

    void render_node_dialog(DbcDoc& doc, Result& res);

    std::map<std::string, Box> boxes_;
    ImVec2                     pan_{};
    float                      zoom_ = 1.0f;
    bool                       fit_  = false;
    Drag                       drag_ = Drag::None;
    std::string                drag_node_;
    std::string                sel_node_name_;
    int                        sel_link_msg_ = -1;   // selected link = (message, receiver)
    std::string                sel_link_to_;
    std::string                pending_from_, pending_to_;   // link being created
    std::string                ctx_from_, ctx_to_;           // link under the right-click
    ImVec2                     ctx_pos_{};

    // "what goes in and out" dialog of a node (double click)
    std::string open_node_;
    int         dlg_out_ = -1, dlg_in_ = -1, dlg_pool_ = -1;
    std::string pool_filter_;
};

template <>
struct std::formatter<DbcNetwork> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const DbcNetwork& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
