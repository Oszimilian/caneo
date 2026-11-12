#pragma once

#include "FileBrowser.hpp"
#include "config/Config.hpp"
#include "xcp/A2lReader.hpp"
#include "xcp/ElfReader.hpp"
#include "xcp/XcpMaster.hpp"

#include <deque>
#include <format>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

class AppState;

// The "XCP" mode: measure and calibrate an ECU over XCP on CAN (FD).
//   top:    target (from the `xcp` blocks of Settings > Channels), connect, ELF / A2L, poll period
//   centre: stacked live plots of measured variables (drag variables in, legend right-click = remove)
//   right:  measured variables (value, poll / DAQ event) and the variable tree
//           (right-click: plot, measure, read, edit; double-click = plot)
// Variables: from the A2L when one is loaded (tree by LONG_IDENTIFIER, units, conversions; addresses from the
// ELF if loaded), otherwise from the ELF.
class XcpView {
public:
    explicit XcpView(AppState& state);
    ~XcpView();

    void render();   // every frame in XCP mode
    void open_elf_dialog() { open_browser_ = Browse::Elf; }
    void open_a2l_dialog() { open_browser_ = Browse::A2l; }
    void render_menu_items();   // "XCP" menu of the menu bar
    void shutdown() { master_.reset(); }   // stops the XCP worker (before the log goes away)

    // Current ELF / measurements / plots (config `xcp:`).
    XcpConfig snapshot() const;

    friend std::ostream& operator<<(std::ostream& os, const XcpView& v);

private:
    struct Series {
        std::vector<double> t, v;
        double last   = 0.0;
        double last_t = 0.0;
        bool   has    = false;
    };
    struct Plot {
        std::vector<std::string> names;
        bool  auto_y = true;    // Y scaled to the visible data; zooming / dragging the Y axis switches it off
        bool  fit_y  = false;   // scale Y once (Fit)
        float rect[4] = {};     // plot area of the last frame (mouse wheel while live)
    };
    struct Row {
        const ElfVariable* var;
        int                depth;
    };

    void apply_config();             // from state_.config().xcp (start / another config loaded)
    void load_elf(const std::string& path);
    void load_a2l(const std::string& path);
    void close_a2l();
    void poll_elf();
    void rebuild_image();             // after the ELF or the A2L changed
    const ElfImage* vars() const { return image_ ? image_.get() : elf_.get(); }   // what the tree shows
    const ElfVariable* find(const std::string& name) const { return vars() ? vars()->find(name) : nullptr; }
    std::string display_name(const std::string& name) const;   // A2L: LONG_IDENTIFIER, else the name
    void connect();
    void push_measurements();
    void measure(const std::string& name, int event);
    void unmeasure(const std::string& name);
    bool measured(const std::string& name) const;
    void add_to_plot(const std::string& name, std::size_t plot);
    void open_edit(const std::string& name);
    void on_sample(const std::string& name, double t, double v);
    const XcpTargetConfig* target() const;
    std::string format_value(const ElfVariable* var, double v) const;
    std::string event_label(int event) const;

    void render_toolbar();
    void render_plots();
    void render_sidebar();
    void render_measured();
    void render_tree();
    void render_var_menu(const ElfVariable& v);
    void render_edit_popup();
    void render_log();

    AppState& state_;

    std::unique_ptr<ElfImage>              elf_;
    std::future<std::unique_ptr<ElfImage>> elf_loading_;
    std::string                            elf_path_;
    std::string                            elf_error_;
    std::unique_ptr<A2lModule>              a2l_;
    std::future<std::unique_ptr<A2lModule>> a2l_loading_;
    std::string                             a2l_path_;
    std::string                             a2l_error_;
    std::unique_ptr<ElfImage>               image_;   // A2L tree (addresses resolved with elf_)

    std::unique_ptr<XcpMaster> master_;
    std::string                target_name_;
    int                        poll_ms_ = 50;
    std::vector<XcpMeasureConfig> measure_;
    std::vector<Plot>          plots_{Plot{}};
    std::size_t                active_plot_ = 0;

    mutable std::mutex            data_mutex_;   // data_, latest_t_ (written by the XCP worker)
    std::map<std::string, Series> data_;
    double                        latest_t_ = 0.0;

    // plots
    static constexpr double DEFAULT_WINDOW_S = 10.0;
    bool   live_     = true;               // follow the newest data
    double window_s_ = DEFAULT_WINDOW_S;   // visible time in live mode
    double x_min_ = 0.0, x_max_ = DEFAULT_WINDOW_S;
    bool   drag_in_plot_ = false;
    void   reset_view();                   // live, default window, Auto Y (also: double-click)
    void   fit_all();                      // all recorded data, paused

    // variable tree
    std::string                     filter_, applied_filter_;
    std::vector<const ElfVariable*> filtered_;
    std::set<std::string>           open_nodes_;
    std::string                     selected_;
    float                           measured_frac_ = 0.35f;

    // edit dialog
    bool        open_edit_ = false;
    std::string edit_name_, edit_text_, edit_error_;

    std::deque<std::string> log_;
    bool                    show_log_ = true;

    enum class Browse { None, Elf, A2l };
    FileBrowser browser_;
    Browse      open_browser_ = Browse::None;
    Browse      browsing_     = Browse::None;
    std::string config_path_;   // config that apply_config() was run for
    bool        applied_ = false;
};

template <>
struct std::formatter<XcpView> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const XcpView& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
