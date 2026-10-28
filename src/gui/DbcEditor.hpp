#pragma once

#include "FileBrowser.hpp"
#include "DbcNetwork.hpp"
#include "dbc/DbcModel.hpp"

#include <format>
#include <ostream>
#include <sstream>
#include <string>

// Full-screen DBC editor (the "DBC" mode): import a DBC, edit nodes, messages, signals
// (layout, scaling, value tables) and write it back.
class AppState;

class DbcEditor {
public:
    explicit DbcEditor(AppState& state) : state_(state) {}

    // Called every frame in DBC mode, after the main menu bar.
    void render();

    void open_file_dialog() { request_import(); }
    // Menu items of the recently used DBC files (inside a menu / popup); a click opens it.
    void render_history_items();

    friend std::ostream& operator<<(std::ostream& os, const DbcEditor& ed);

private:
    enum class Browse { None, Import, SaveAs, New };

    void render_toolbar();
    void render_messages();
    void render_nodes();
    void render_message(DbcMessage& m);
    void render_bit_layout(DbcMessage& m);
    void render_signal_table(DbcMessage& m);
    void render_signal(DbcMessage& m, DbcSignal& s);
    void render_node(std::size_t idx);
    void render_node_tree(const std::string& node);
    void render_dialogs();

    void request_import();
    void load_path(const std::string& path);
    void open_path(const std::string& path);   // asks first when there are unsaved changes
    void remember(const std::string& path);
    void new_document();
    void save_to(const std::string& path);
    void select_message(int idx);

    AppState&   state_;
    std::string pending_open_;   // history entry waiting for the "discard" answer
    DbcDoc      doc_;
    std::string path_;
    bool        dirty_ = false;
    std::string error_;

    int  sel_msg_  = -1;
    int  sel_sig_  = -1;
    int  sel_node_ = -1;
    bool node_focus_ = false;   // the right panel shows the node instead of the message
    std::string filter_;

    bool       network_view_ = false;   // node graph instead of the message table
    DbcNetwork network_;

    FileBrowser browser_;
    Browse      browse_  = Browse::None;
    Browse      pending_ = Browse::None;
    bool        ask_discard_ = false;   // "unsaved changes" popup requested
    Browse      after_discard_ = Browse::None;
};

template <>
struct std::formatter<DbcEditor> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const DbcEditor& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
