#pragma once

#include "Window.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

class GraphWindow : public Window {
public:
    GraphWindow();

    void update(const CanFrame& frame) override;

    // Thread-safe: push a value directly (for non-CAN sources like model outputs).
    // Uses "interface" + msg_id=0 as the series key.
    void push_value(const std::string& interface,
                    const std::string& signal_name,
                    double             value);

    void render() override;

    // Called from render thread only:
    size_t add_graph();
    void   add_signal(size_t graph_idx,
                      const std::string& interface,
                      uint32_t           msg_id,
                      const std::string& signal_name,
                      double             y_min,
                      double             y_max,
                      int                y_axis = 0); // 0=Y1, 1=Y2, 2=Y3
    std::vector<std::string> graph_names() const;

private:
    struct SignalSeries {
        std::string interface;
        uint32_t    msg_id;
        std::string signal_name;
        double      y_min  = 0.0;
        double      y_max  = 0.0;
        int         y_axis = 0;   // 0=Y1, 1=Y2, 2=Y3

        static constexpr size_t MAX = 10000;
        std::vector<double> xs;
        std::vector<double> ys;

        void push(double t, double v) {
            xs.push_back(t);
            ys.push_back(v);
            if (xs.size() > MAX) {
                xs.erase(xs.begin());
                ys.erase(ys.begin());
            }
        }
    };

    struct GraphTab {
        std::string               name;
        std::vector<SignalSeries> series;
        bool                      follow  = true;
        double                    x_range = 30.0; // visible X window width in seconds
    };

    struct PendingSeriesCtx {
        size_t      tab_idx;
        size_t      series_idx;
        std::string signal_name;
        int         current_axis;
    };

    std::vector<GraphTab>                  graphs_;
    mutable std::mutex                     mutex_;
    std::chrono::steady_clock::time_point  start_;

    std::optional<PendingSeriesCtx>        pending_series_ctx_;
    bool                                   open_series_ctx_ = false;
};
