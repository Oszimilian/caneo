#pragma once

#include "Window.hpp"
#include "config/Config.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class GraphWindow : public Window {
public:
    // Interface name used for values pushed via push_value() (model outputs).
    static constexpr const char* MODEL_IFACE = "__model__";
    // Interface name for guard outputs ("<guard>/<output>").
    static constexpr const char* GUARD_IFACE = "__guards__";
    // msg_id of series whose message was not found in the DBC.
    static constexpr uint32_t    UNRESOLVED  = std::numeric_limits<uint32_t>::max();

    GraphWindow();

    const char* title() const override { return "Graphs"; }

    void update(const CanFrame& frame) override;

    // Thread-safe: push a value directly (for non-CAN sources like model outputs).
    // Uses "interface" + msg_id=0 as the series key.
    void push_value(const std::string& interface,
                    const std::string& signal_name,
                    double             value);

    void render() override;

    // Called from render thread only:
    size_t add_graph(std::string name = {}, double window_s = DEFAULT_WINDOW_S, bool auto_y = true);
    void   add_signal(size_t             graph_idx,
                      const std::string& interface,
                      uint32_t           msg_id,
                      const std::string& msg_name,
                      const std::string& signal_name,
                      double             y_min,
                      double             y_max,
                      int                y_axis = 0,      // 0=Y1, 1=Y2, 2=Y3
                      std::optional<double> cfg_y_min = {},
                      std::optional<double> cfg_y_max = {});
    std::vector<std::string> graph_names() const;

    // Removes all graphs (used when a config is loaded).
    void clear();

    // Current graphs in config form, for saving to caneo.yaml.
    std::vector<GraphConfig> export_config() const;

    static constexpr double DEFAULT_WINDOW_S = 30.0;

private:
    // Fixed-capacity ring buffer of (t, value) samples, oldest first.
    struct RingBuffer {
        static constexpr size_t CAPACITY = 200'000;

        std::vector<double> xs, ys;
        size_t              head = 0;   // index of the oldest sample once full

        void   push(double t, double v);
        void   clear() { xs.clear(); ys.clear(); head = 0; }
        size_t size() const { return xs.size(); }
        bool   empty() const { return xs.empty(); }
        double x(size_t i) const { return xs[(head + i) % xs.size()]; }
        double y(size_t i) const { return ys[(head + i) % ys.size()]; }
        // First logical index with x(i) >= t.
        size_t lower_bound(double t) const;
    };

    struct SignalSeries {
        std::string interface;
        uint32_t    msg_id;
        std::string msg_name;
        std::string signal_name;
        std::string label;        // legend label
        double      y_min  = 0.0; // DBC range (0/0 = unknown)
        double      y_max  = 0.0;
        int         y_axis = 0;   // 0=Y1, 1=Y2, 2=Y3
        std::optional<double> cfg_y_min, cfg_y_max; // from config, kept for saving

        RingBuffer data;
        uint32_t   color = 0;     // line color of the last frame (for the tooltip)

        // Per-frame decimated copy of the visible range (render thread only).
        std::vector<double> px, py;
    };

    struct GraphTab {
        std::string               name;
        std::vector<SignalSeries> series;
        bool                      follow   = true;
        bool                      auto_y   = true;
        double                    x_range  = DEFAULT_WINDOW_S; // visible width while following
        double                    default_range = DEFAULT_WINDOW_S;
        double                    view_x0  = 0.0;  // X limits of the last frame
        double                    view_x1  = DEFAULT_WINDOW_S;
        bool                      init_y   = true; // apply DBC Y range once
        std::optional<std::pair<double, double>> set_x; // one-shot X limits (Fit)
        bool                      fit_y    = false;      // one-shot Y fit
        bool                      drag_in_plot = false;  // mouse pressed inside the plot area
        float                     plot_rect[4] = {};     // plot area of the last frame (x0,y0,x1,y1)
    };

    struct PendingSeriesCtx {
        size_t      tab_idx;
        size_t      series_idx;
        std::string label;
        int         current_axis;
    };

    size_t add_graph_unlocked(std::string name, double window_s, bool auto_y);
    void render_toolbar(GraphTab& tab);
    void render_plot(size_t ti, GraphTab& tab);
    void render_series_popup();
    void reset_view(GraphTab& tab);
    void fit_all(GraphTab& tab);

    std::vector<GraphTab>                  graphs_;
    mutable std::mutex                     mutex_;
    std::chrono::system_clock::time_point  start_;

    std::optional<PendingSeriesCtx>        pending_series_ctx_;
    bool                                   open_series_ctx_ = false;

    // Tab context menu (rename / delete)
    std::optional<size_t>                  rename_idx_;
    char                                   rename_buf_[128] = {};
    std::optional<size_t>                  delete_idx_;
};
