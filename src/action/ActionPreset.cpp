#include "ActionPreset.hpp"
#include "Action.hpp"
#include "send/SendModel.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

// ── helpers ───────────────────────────────────────────────────────────────────

static int find_msg_idx(const SendModel& model, const std::string& name)
{
    const auto& msgs = model.messages();
    for (int i = 0; i < (int)msgs.size(); ++i)
        if (msgs[i].name == name)
            return i;
    return -1;
}

static int find_sig_idx(const SendMessage& msg, const std::string& name)
{
    for (int k = 0; k < (int)msg.signals.size(); ++k)
        if (msg.signals[k].name == name)
            return k;
    return -1;
}

// Apply signal values from a YAML map node to the model, return base payload.
static std::vector<uint8_t> apply_signals_and_encode(
    SendModel&          model,
    int                 msg_idx,
    const YAML::Node&   signals_node)
{
    const auto& msg = model.messages()[msg_idx];
    if (signals_node && signals_node.IsMap()) {
        for (const auto& kv : signals_node) {
            const int k = find_sig_idx(msg, kv.first.as<std::string>());
            if (k >= 0)
                model.set_value(msg_idx, k, kv.second.as<double>());
        }
    }
    return model.encode(msg_idx);
}

// ── loader ────────────────────────────────────────────────────────────────────

void load_action_presets(const InterfaceConfig& cfg, ActionHandler& handler)
{
    if (cfg.actions_file.empty() || cfg.dbc.empty())
        return;

    YAML::Node root;
    try {
        root = YAML::LoadFile(cfg.actions_file);
    } catch (const std::exception& e) {
        throw std::runtime_error(
            std::string("Failed to load actions file '") + cfg.actions_file + "': " + e.what());
    }

    if (!root.IsSequence())
        throw std::runtime_error("Actions file must be a YAML sequence (list of action entries)");

    // One SendModel per preset file load — shared across all sin encode_fns
    // so that the INetwork (and its ISignal pointers) stays alive.
    auto model = std::make_shared<SendModel>(cfg.dbc);

    for (const auto& entry : root) {
        const std::string type = entry["type"] ? entry["type"].as<std::string>() : "";
        const std::string msg_name = entry["msg"] ? entry["msg"].as<std::string>() : "";

        const int msg_idx = find_msg_idx(*model, msg_name);
        if (msg_idx < 0)
            continue;  // message not found, skip silently

        const auto& msg = model->messages()[msg_idx];

        if (type == "periodic") {
            const int period_ms = entry["period_ms"] ? entry["period_ms"].as<int>() : 100;
            auto payload = apply_signals_and_encode(*model, msg_idx, entry["signals"]);

            handler.add_action_paused(std::make_unique<PeriodicAction>(
                handler.io_ref(), cfg.name,
                msg.id, msg.name,
                std::move(payload),
                handler.send_fn_ref(),
                std::chrono::milliseconds(period_ms)));

        } else if (type == "sin") {
            const std::string sig_name = entry["signal"] ? entry["signal"].as<std::string>() : "";
            const int sig_idx = find_sig_idx(msg, sig_name);
            if (sig_idx < 0)
                continue;  // signal not found, skip

            const double amplitude   = entry["amplitude"]     ? entry["amplitude"].as<double>()     : 1.0;
            const double offset      = entry["offset"]        ? entry["offset"].as<double>()        : 0.0;
            const int    period_ms   = entry["sin_period_ms"] ? entry["sin_period_ms"].as<int>()    : 1000;
            const int    interval_ms = entry["interval_ms"]   ? entry["interval_ms"].as<int>()      : 20;

            // Encode base payload with all non-sin signals set to preset values
            auto base_payload = apply_signals_and_encode(*model, msg_idx, entry["signals"]);

            const auto& ssig = msg.signals[sig_idx];
            const double y_lo = ssig.min_val;
            const double y_hi = ssig.max_val;
            const dbcppp::ISignal* isig = ssig.sig;

            // Capture model shared_ptr to keep INetwork (and ISignal ptrs) alive
            SinPeriodicAction::EncodeWithValueFn encode_fn =
                [model, base_payload, isig, y_lo, y_hi](double v) mutable {
                    if (y_lo < y_hi)
                        v = std::clamp(v, y_lo, y_hi);
                    auto data = base_payload;
                    const auto raw = isig->PhysToRaw(v);
                    isig->Encode(raw, data.data());
                    return data;
                };

            handler.add_action_paused(std::make_unique<SinPeriodicAction>(
                handler.io_ref(), cfg.name,
                msg.id, msg.name,
                handler.send_fn_ref(),
                std::chrono::milliseconds(interval_ms),
                std::move(encode_fn),
                amplitude,
                std::chrono::milliseconds(period_ms),
                offset));
        }
        // Unknown type → skip silently
    }
}
