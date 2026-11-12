#pragma once

#include "AppState.hpp"
#include "FileBrowser.hpp"
#include "ImNodesPan.hpp"
#include "Window.hpp"
#include "flow/FlowEngine.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

struct ImNodesContext;
struct ImNodesEditorContext;

// What the editor needs from its surroundings: the flow list (live engine, or the
// offline flows of the Debug view) and the messages/signals the CAN blocks can pick.
class FlowBackend {
public:
    struct Msg {
        std::string              name;
        std::vector<std::string> signals;
    };

    virtual ~FlowBackend() = default;

    virtual std::vector<FlowInfo> snapshot() const                                   = 0;
    virtual void                  add(const FlowSpec& spec)                          = 0;
    virtual void                  replace(std::size_t idx, const FlowSpec& spec)     = 0;
    virtual void                  remove(std::size_t idx)                            = 0;
    virtual void                  set_enabled(std::size_t idx, bool enabled)         = 0;

    virtual std::vector<std::string> interfaces() const                              = 0;
    // Valid until the next call.
    virtual const std::vector<Msg>&  messages(const std::string& iface)              = 0;

    // Offline: flows run over a loaded log; no CAN Out blocks, no live values.
    virtual bool offline() const { return false; }
    virtual void trigger(std::size_t /*flow*/, int /*node_id*/) {}   // live: send a can_out block now
    virtual void set_control(std::size_t /*flow*/, int /*node_id*/, double /*value*/) {}   // live: slider value / button press
    // Offline: writes the table of a csv block (from the last run) to path. Returns a status text.
    virtual std::string export_csv(std::size_t /*flow*/, int /*node_id*/, const std::string& /*path*/) {
        return "not available";
    }
};

// Node editor (imnodes) for flows: CAN signals in -> Lua / generator blocks ->
// CAN messages / scopes out. Edits a working copy; "Apply" (re)starts the flow.
class FlowEditorWindow : public Window {
public:
    explicit FlowEditorWindow(AppState& state);   // live flows
    FlowEditorWindow(AppState& state, std::unique_ptr<FlowBackend> backend, std::string title);
    ~FlowEditorWindow() override;

    const char* title() const override { return title_.c_str(); }
    void render() override;            // as a window of its own
    void render_contents();            // toolbar + canvas, to embed in another window

    // Embedded use for one flow: no flow chooser / New / Delete in the toolbar.
    void set_compact(bool compact) { compact_ = compact; }
    void open_flow(std::size_t idx) { select(idx); }
    void start_new_flow() { new_flow(); }
    int  current_flow() const { return has_edit_ ? edit_idx_ : -1; }   // index of the shown flow, -1 = none / unsaved
    // A flow was deleted elsewhere: forget it / shift the index.
    void flow_removed(std::size_t idx) {
        if (edit_idx_ == static_cast<int>(idx)) { has_edit_ = false; edit_idx_ = -1; }
        else if (edit_idx_ > static_cast<int>(idx)) --edit_idx_;
    }
    // Creates a flow from a ready-made diagram, starts it and shows it.
    void create_flow(FlowSpec spec);
    bool has_flow() const { return has_edit_; }
    const std::string& flow_name() const { return edit_.name; }

    // Series name of a scope block in the graph window / log.
    static std::string series_name(const std::string& flow, const std::string& scope) {
        return "flow/" + flow + "/" + scope;
    }

private:
    void select(std::size_t idx);
    void new_flow();
    void apply();
    bool dirty(const FlowSpec& applied) const;

    void render_toolbar(const std::vector<FlowInfo>& flows);
    void render_canvas(const FlowInfo* info);
    void render_node(FlowNodeSpec& node, std::size_t node_idx, const FlowState::Values* live);
    void render_add_popup();
    void render_plot_popup();

    void add_node(const std::string& type, float x, float y);
    void remove_node(int id);
    void set_signals(FlowNodeSpec& node, std::vector<std::string> signals);
    int  node_index(int id) const;

    bool msg_combo(FlowNodeSpec& node, const std::vector<FlowBackend::Msg>& msgs);
    bool signal_popup(FlowNodeSpec& node, const std::vector<FlowBackend::Msg>& msgs);

    AppState&                    state_;
    std::unique_ptr<FlowBackend> backend_;
    std::string                  title_;
    ImNodesContext*       ctx_ = nullptr;
    ImNodesEditorContext* editor_ = nullptr;   // own editor: CreateContext() only sets the default one on the *current* context
    std::string     theme_;
    ImNodesZoom     zoom_;

    FlowSpec    edit_;
    FlowSpec    applied_;
    bool        has_edit_ = false;
    int         edit_idx_ = -1;          // index in the engine, -1 = not applied yet
    bool        positions_dirty_ = true; // push edit_ positions into imnodes
    int         next_id_ = 1;
    bool        show_log_ = false;
    bool        compact_  = false;

    bool        open_add_popup_ = false;
    float       add_x_ = 0.0f, add_y_ = 0.0f;
    std::string plot_series_;
    bool        open_plot_popup_ = false;
    std::string msg_filter_;
    FileBrowser csv_browser_;
    int         csv_browse_node_ = 0;   // block whose path is being chosen
    bool        open_csv_browser_ = false;
    std::map<int, std::string> csv_status_;   // last export result per csv block
};
