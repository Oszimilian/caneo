#pragma once

#include "GuiDataFrameSet.hpp"
#include "action/ActionHandler.hpp"
#include "config/Config.hpp"
#include "frame/DataFrameSet.hpp"
#include "playback/PlaybackController.hpp"
#include "send/SendModel.hpp"

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <chrono>
#include <deque>
#include <optional>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class GuardEngine;
struct GuardInfo;
class ILogControl;

class TuiDataFrameSet : public GuiDataFrameSet {
public:
    TuiDataFrameSet(const std::vector<InterfaceConfig>& iface_configs,
                    ActionHandler&                      action_handler,
                    PlaybackController*                 playback_ctrl = nullptr);

    void update(const CanFrame& frame) override;
    void run() override;

    void set_status_indicator(std::string label, std::function<bool()> connected_fn);

    // Model tab: call enable_model_tab() before run(), then push_model_output()
    // from the asio thread (e.g. via ModelEngine::set_output_callback).
    void enable_model_tab();
    void push_model_output(const std::string& name, double value);

    // Guards tab ('g'): list with live input/outputs, Space toggles a guard.
    void set_guard_engine(GuardEngine* engine) { guard_engine_ = engine; }

    // Logging ('l' start with path prompt / stop, 'o' open in viewer).
    // config provides the default directory, the format and the viewer
    // command; both must outlive the TUI.
    void set_log_control(ILogControl* log, const Config* config);

    // Graphs tab ('v'): presets from the config (graphs:); signals are added
    // from Trace (Enter on a signal), Guards and Model. Call before run().
    void set_graphs(const std::vector<GraphConfig>& graphs);
    // Guard output for the graphs (asio thread), series "<guard>/<output>".
    void push_guard_output(const std::string& guard, const std::string& output, double value);

private:
    enum class Tab { Trace, Send, Actions, Guards, Model, Playback, Graphs };

    // ── Mouse: elements register their screen box + a click handler while
    // rendering; a click runs the innermost (last registered) target.
    struct ClickTarget {
        ftxui::Box            box;
        std::function<void()> fn;
        bool                  keep_modes;   // don't cancel text input / dialogs
    };
    ftxui::Element clickable(ftxui::Element e, std::function<void()> fn, bool keep_modes = false);
    ftxui::Element button(const std::string& label, std::function<void()> fn,
                          bool highlight = false, bool keep_modes = false);
    ftxui::Element tab_bar(const std::vector<std::string>& labels, int selected, bool focused,
                           std::function<void(int)> on_click);
    bool handle_mouse(const ftxui::Mouse& mouse);
    void emit(const ftxui::Event& event);   // runs the key handler, e.g. for buttons
    std::function<bool(ftxui::Event)> key_handler_;
    void scroll(int delta);
    void cancel_modes();          // leave text input / action creation
    void select_tab(Tab tab);
    void toggle_log();            // start prompt / stop
    void open_viewer();

    // ── Graphs ────────────────────────────────────────────────────────────
    struct GraphSeries {
        std::string                          iface;   // empty = any; MODEL_IFACE / GUARD_IFACE
        std::string                          msg;     // empty for model / guard outputs
        std::string                          signal;
        std::optional<double>                y_min, y_max;
        std::deque<std::pair<double, double>> pts;    // (seconds since start, value)
    };
    struct GraphView {
        std::string              name;
        double                   window_s = 30.0;
        bool                     auto_y   = true;
        std::vector<GraphSeries> series;
    };
    static constexpr const char* MODEL_IFACE = "__model__";
    static constexpr const char* GUARD_IFACE = "__guards__";
    double now_s() const;
    void   record(const std::string& iface, const std::string& msg, const std::string& signal,
                  double value, double t);                 // mutex_ held
    // Adds the series to the current graph, or removes it if present.
    void   toggle_graph_signal(const std::string& iface, const std::string& msg,
                               const std::string& signal);  // locks mutex_
    bool   graphed(const std::string& iface, const std::string& msg,
                   const std::string& signal) const;        // mutex_ held
    GraphView& current_graph();                             // mutex_ held
    void   graph_guard_outputs(const GuardInfo& guard);     // adds all outputs; locks mutex_
    void   trace_toggle_selected_signal();                  // locks mutex_
    ftxui::Element render_graphs();
    ftxui::Element render_plot(const GraphView& g);

    // Trace rows in display order (matches first while searching); mutex_ held
    std::vector<uint32_t> trace_order(const DataFrameSet& set) const;
    const DataFrameSet*   active_trace_set() const;        // mutex_ held
    std::vector<Tab> tabs() const;   // visible main tabs in display order
    bool handle_log_event(const ftxui::Event& event);
    void set_message(std::string msg, bool error = false);
    std::string log_files(const std::string& path) const;

    // Rendering
    ftxui::Element render();
    ftxui::Element render_trace();
    ftxui::Element render_send();
    ftxui::Element render_msg_list(const SendModel& model);
    ftxui::Element render_sig_list(const SendModel& model);
    ftxui::Element render_actions();
    ftxui::Element render_action_signals(const ActionInfo& info);
    ftxui::Element render_playback();
    ftxui::Element render_model();
    ftxui::Element render_guards();
    ftxui::Element render_action_log(const ActionInfo& info);
    ftxui::Element render_status_line();

    // Helpers
    const std::string& selected_send_iface() const;
    SendModel*         selected_send_model();
    const SendModel*   selected_send_model() const;

    void create_single_action();
    void create_periodic_action(std::chrono::milliseconds period);
    void create_sin_action(double amplitude, std::chrono::milliseconds sin_period, double offset);

    // Interface + send models
    std::vector<std::string>                          interfaces_;
    std::map<std::string, std::unique_ptr<SendModel>> send_models_;

    // Action handler (externally owned)
    ActionHandler& action_handler_;

    // Playback controller (null when not in playback mode)
    PlaybackController* playback_ctrl_ = nullptr;

    // ── Navigation (ftxui thread only) ───────────────────────────────────
    // nav_level_: 0=MainTabs  1=SubTabs/ActionList  2=MsgList  3=SigList
    int  nav_level_          = 0;
    Tab  main_tab_           = Tab::Trace;
    int  playback_cursor_    = 0; // selected interface in playback tab
    int  playback_msg_cursor_ = 0; // 0=All, 1..N=individual message
    int  sub_tab_trace_   = 0;
    int  sub_tab_send_    = 0;
    int  send_msg_cursor_ = 0;
    int  send_sig_cursor_ = 0;
    int  actions_cursor_    = 0;
    int  actions_sig_cursor_ = 0; // cursor in signal edit view (Actions tab)

    // Signal value editing (Send tab)
    bool        send_editing_      = false;
    std::string send_edit_buf_;

    // Signal value editing (Actions tab)
    bool        actions_editing_    = false;
    std::string actions_edit_buf_;

    // Periodic / Sin action creation state machine
    enum class PeriodicStep { None, Period, TypeSelect, SinAmplitude, SinPeriod, SinOffset, SinReady };
    PeriodicStep periodic_step_        = PeriodicStep::None;
    std::string  periodic_interval_buf_;
    long         periodic_interval_ms_ = 0;
    int          periodic_type_cursor_ = 0;  // 0 = Constant, 1 = Sin
    std::string  sin_amplitude_buf_;
    std::string  sin_period_buf_;
    std::string  sin_offset_buf_;

    // ── Trace search & scroll (ftxui thread only) ────────────────────────
    bool        trace_searching_   = false;
    std::string trace_search_buf_;
    int         trace_cursor_      = 0;
    int         trace_sig_cursor_  = 0;   // nav_level_ 2: signal of the selected frame

    // ── Guards tab ────────────────────────────────────────────────────────
    GuardEngine* guard_engine_  = nullptr;
    int          guards_cursor_ = 0;

    // ── Logging ───────────────────────────────────────────────────────────
    ILogControl*  log_          = nullptr;
    const Config* config_       = nullptr;
    bool          log_prompt_   = false;   // editing the log file path
    std::string   log_path_buf_;

    // One-line feedback below the tabs (log started, viewer error, ...)
    std::string                           message_;
    bool                                  message_error_ = false;
    std::chrono::steady_clock::time_point message_time_{};

    // ── Mouse targets of the last render ─────────────────────────────────
    std::deque<ClickTarget> click_targets_;

    // ── Graphs (series data guarded by mutex_) ─────────────────────────────
    std::vector<GraphView>                graphs_;
    int                                   graph_tab_    = 0;
    int                                   graph_cursor_ = 0;   // legend entry
    bool                                  graph_paused_ = false;
    double                                graph_pause_t_ = 0.0;
    std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();

    // ── gRPC status indicator ─────────────────────────────────────────────
    std::string           status_label_;
    std::function<bool()> status_connected_fn_;

    // ── Model tab ─────────────────────────────────────────────────────────
    bool                          model_tab_enabled_ = false;
    std::map<std::string, double> model_outputs_;  // guarded by mutex_

    // ── Trace data (guarded by mutex_) ────────────────────────────────────
    mutable std::mutex                  mutex_;
    std::map<std::string, DataFrameSet> sets_;

    ftxui::ScreenInteractive screen_ = ftxui::ScreenInteractive::Fullscreen();
};
