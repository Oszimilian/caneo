#pragma once

#include "FileBrowser.hpp"
#include "FlowEditorWindow.hpp"
#include "config/Config.hpp"
#include "debug/DbcInfo.hpp"
#include "debug/LogDocument.hpp"
#include "debug/OfflineFlow.hpp"

#include <format>
#include <imgui.h>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <ostream>
#include <string>
#include <vector>

class AppState;

// Full-screen offline log viewer (the "Debug" mode): opens MCAP / BLF files and
// plots their signals, similar to PlotJuggler. BLF files need a DBC per
// interface because they only contain raw frames.
class DebugView {
public:
    explicit DebugView(AppState& state);
    ~DebugView();   // out of line: FlowEditorWindow members

    // Called every frame in Debug mode, after the main menu bar.
    void render();

    void open_file_dialog() { browse_ = Browse::Log; }

    // Current log + plots (stored in the config history on save), if a log is loaded.
    std::optional<DebugSession> snapshot() const;
    // Reloads a stored session: log, DBCs and plots.
    void restore(const DebugSession& session);
    // Menu items of the stored sessions (inside a menu / popup); a click restores it.
    void render_history_items();

    friend std::ostream& operator<<(std::ostream& os, const DebugView& view);

private:
    enum class Browse { None, Log, Dbc, Csv };

    struct Plot {
        std::vector<std::size_t> series;   // indices into LogDocument::Data::series
        bool fit_x = false, fit_y = false; // one-shot: fit the whole log / the data (set when a signal is added)
        std::optional<std::pair<double, double>> zoom_x, zoom_y;   // one-shot limits from the right-drag zoom
        double ya = 0.0, yb = 0.0;         // horizontal measurement cursors
        bool   y_init = false;
    };
public:
    // One tab of the plot area: its own stack of plots.
    struct PlotTab {
        std::string       name;
        std::vector<Plot> plots{Plot{}};
        std::size_t       active_plot = 0;
        bool              fit_requested = true;
        std::vector<float> row_ratios;   // subplot heights, reset by "Default"
        bool              xy = false;           // XY plot: plots[0].series = {x, y}
        bool              xy_equal = true;      // equal axis scale
        // XY points: y resampled (linear) onto the sample times of x; rebuilt when the key changes
        std::vector<double> xy_x, xy_y;
        std::vector<std::size_t> xy_key;

        // View history (zoom / pan) for "Undo": one entry per burst of changes.
        struct View {
            double x0 = 0.0, x1 = 0.0;
            std::vector<std::pair<double, double>> y;
            bool has_y = false;   // false while Auto-Y drives the Y axes
        };
        std::vector<View> undo;
        View              last;                 // view of the previous frame
        View              burst_start;          // view before the running burst of changes
        bool              last_valid = false, in_burst = false;
        double            last_change = 0.0;
        int               skip_frames = 0;      // after an undo: adopt the view without recording
    };
private:

    void on_data_changed();
    void rebuild_tree();
    // Runs the enabled offline flows over the loaded log and adds their scopes as series.
    void apply_flows(bool keep_plots);
    void render_sidebar();
    void render_files();
    void render_signal_tree(float height);
    void render_flow_list();
    void open_flow_tab(std::size_t idx);
    void new_flow();
    void delete_flow(std::size_t idx);
    void render_plots(PlotTab& tab);
    void render_xy(PlotTab& tab);
    void render_plot_tabs();
    void render_dbc_viewer();
    void render_unmatched();
    void render_spectrum();
    void view_dbc(const std::string& path);
    void render_plot_toolbar();
    void record_view(PlotTab& tab, const PlotTab::View& now);
    void undo_view(PlotTab& tab);
    void default_view(PlotTab& tab);
    void render_dialogs();
    void add_signal(std::size_t plot, std::size_t series_idx);
    void load(const std::string& path);

    AppState&   state_;
    LogDocument doc_;
    FileBrowser browser_;
    Browse      browse_ = Browse::None;    // requested by the panels
    Browse      pending_ = Browse::None;   // the dialog that is open
    std::string browse_iface_;

    std::string                        log_path_;
    std::map<std::string, std::string> dbcs_;        // interface -> DBC path
    std::string                        filter_;

    // signal tree, rebuilt when data changes: iface -> msg -> series indices
    std::map<std::string, std::map<std::string, std::vector<std::size_t>>> tree_;

    std::vector<PlotTab> tabs_{PlotTab{"Plot 1"}};
    std::size_t          current_tab_ = 0;
    int                  tab_counter_ = 1;
    std::size_t          rename_tab_  = 0;
    std::string          rename_buf_;
    bool                 open_rename_ = false;
    bool              auto_y_       = false;
    bool              stairs_       = false;
    bool              cursor_valid_ = false;   // a plot was hovered last frame
    double            cursor_x_     = 0.0;
    int               zoom_plot_    = -1;      // plot with a right-drag zoom in progress
    ImVec2            zoom_start_{};           // drag start (screen)
    double            zoom_x0_ = 0.0, zoom_y0_ = 0.0;   // drag start (plot coordinates)
    bool              meas_x_       = false;   // vertical measurement cursors A / B
    bool              meas_y_       = false;   // horizontal measurement cursors per plot
    bool              meas_x_init_  = false;
    double            xa_ = 0.0, xb_ = 0.0;
    double            view_x0_ = 0.0, view_x1_ = 1.0;   // X range of the first plot (places new cursors)
    double            last_record_  = 0.0;   // ImGui time of the last history update

    std::vector<double> px_, py_;   // decimation scratch

    // Spectrum window: FFT of the signals of the active plot over the cursor / visible range
    struct Spec {
        std::string         label, unit;
        std::vector<double> f, a;   // frequency [Hz], amplitude
        double              peak_f = 0.0, peak_a = 0.0, fs = 0.0;
        std::size_t         samples = 0, nfft = 0;
    };
    bool                    spectrum_open_ = false;
    bool                    spec_log_x_    = false;
    bool                    spec_db_       = false;
    std::vector<Spec>       spec_;
    std::vector<std::size_t> spec_key_series_;
    double                  spec_key_a_ = -1.0, spec_key_b_ = -1.0;
    unsigned                spec_key_version_ = ~0u;
    std::vector<double>     spec_px_, spec_py_;
    bool                    spec_fit_ = true;   // fit the axes on the next frame

    // DBC viewer
    bool                   dbc_open_ = false;
    std::optional<DbcInfo> dbc_info_;
    std::string            dbc_error_;
    std::string            dbc_filter_;
    bool                   unmatched_open_ = false;

    // Offline flows (config debug_flows): editor in an extra column, results appended to the log's series
    struct FlowTab {
        int                               id = 0;
        std::size_t                       idx = 0;   // flow in config().debug_flows
        std::unique_ptr<FlowEditorWindow> editor;
    };
    std::vector<FlowTab>                        flow_tabs_;
    int                                         flow_tab_counter_ = 0;
    int                                         focus_flow_tab_ = 0;   // tab id to select once
    std::vector<std::vector<CsvTable>>          flow_csv_;      // csv blocks' tables per flow
    std::vector<std::shared_ptr<FlowState>>     flow_states_;   // log / error per flow, same order as the specs
    unsigned                                    data_version_ = 0;
    std::size_t                                 xy_epoch_ = 0;   // bumped when series values may have changed
    float                                       flows_frac_ = 0.3f;   // height share of the flow list in the sidebar

    std::optional<DebugSession> restore_;   // plots to rebuild once the log is loaded
};

template <>
struct std::formatter<DebugView> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const DebugView& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
