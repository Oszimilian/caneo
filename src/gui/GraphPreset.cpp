#include "GraphPreset.hpp"

void apply_graph_config(const Config& config, GraphWindow& graph_window, const ModelLookup& models)
{
    for (const auto& graph : config.graphs) {
        const size_t graph_idx = graph_window.add_graph(graph.name, graph.window_s, graph.auto_y);

        for (const auto& sig : graph.signals) {
            if (sig.msg.empty()) {
                // Model / guard output: pushed by name, no DBC involved.
                const std::string iface = sig.interface == GraphWindow::GUARD_IFACE
                                        ? GraphWindow::GUARD_IFACE : GraphWindow::MODEL_IFACE;
                graph_window.add_signal(graph_idx, iface, 0, {}, sig.signal,
                                        0.0, 0.0, sig.y_axis, sig.y_min, sig.y_max);
                continue;
            }

            auto try_iface = [&](const std::string& iface) -> bool {
                const auto model = models(iface);
                if (!model) return false;
                const int mi = model->find_message(sig.msg);
                if (mi < 0) return false;
                const auto& msg = model->messages()[mi];
                double y_min = 0.0, y_max = 0.0;
                if (const int si = model->find_signal(mi, sig.signal); si >= 0) {
                    y_min = msg.signals[si].min_val;
                    y_max = msg.signals[si].max_val;
                }
                graph_window.add_signal(graph_idx, iface, static_cast<uint32_t>(msg.id), sig.msg, sig.signal,
                                        y_min, y_max, sig.y_axis, sig.y_min, sig.y_max);
                return true;
            };

            bool found = false;
            if (!sig.interface.empty()) {
                found = try_iface(sig.interface);
            } else {
                for (const auto& iface : config.interfaces)
                    if ((found = try_iface(iface.name))) break;
            }
            if (!found)
                graph_window.add_signal(graph_idx, sig.interface, GraphWindow::UNRESOLVED, sig.msg, sig.signal,
                                        0.0, 0.0, sig.y_axis, sig.y_min, sig.y_max);
        }
    }
}
