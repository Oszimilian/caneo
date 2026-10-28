#pragma once

#include "config/Config.hpp"
#include "flow/FlowEngine.hpp"
#include "send/ModelCache.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

class GraphWindow;

// GUI-side owner of the configuration (render thread only).
//
// Holds two configs: the one caneo was started with (sockets, decoders and
// trace windows are built from it) and the edited one shown in Settings.
// Actions and graphs are live objects; collect() merges their current state
// into the config for saving. Channel changes need a restart.
class AppState {
public:
    AppState(Config config, std::string path, ModelCache& models, FlowEngine& flows);

    Config&            config()        { return config_; }
    const Config&      running() const { return running_; }
    const std::string& path() const    { return path_; }

    ModelCache&    models()  { return models_; }
    FlowEngine&    flows()   { return flows_; }

    // Sends one frame (the Send window); may be called from the GUI thread.
    void set_send_fn(SendFn fn) { send_fn_ = std::move(fn); }
    void send(const std::string& iface, uint64_t id, const std::vector<uint8_t>& data) const {
        if (send_fn_) send_fn_(iface, id, data);
    }

    void set_graph_window(GraphWindow* gw) { graph_window_ = gw; }
    GraphWindow* graph_window() const      { return graph_window_; }

    // True if interfaces/DBCs/baudrates/vcan differ from the running setup.
    bool channels_changed() const { return !config_.same_channels(running_); }

    // The Debug view registers a function returning its current session (empty if
    // no log is loaded). save() and autosave store it in config().debug_history.
    void set_debug_snapshot(std::function<std::optional<DebugSession>()> fn) { debug_snapshot_ = std::move(fn); }

    // Stores the Debug view's current session in config().debug_history (newest first).
    void record_debug_session();

    // Edited config + current actions + current graphs.
    Config collect() const;

    // Save to path (and remember it). Returns false on error, see status().
    bool save(const std::string& path);
    bool save() { return save(path_); }

    // Loads a config file: graphs, actions and GUI settings are applied
    // immediately, channel settings only after a restart.
    bool load(const std::string& path);

    // Call every frame with the current time (s): saves to path() every
    // config().gui.autosave_s seconds if something changed since the last save.
    void autosave_tick(double now);
    double last_autosave() const { return last_autosave_time_; }

    void request_restart() { restart_requested_ = true; }
    bool restart_requested() const { return restart_requested_; }

    // Last result message for the status line.
    const std::string&              status() const       { return status_; }
    bool                            status_error() const { return status_error_; }
    const std::vector<std::string>& warnings() const     { return warnings_; }
    void set_status(std::string msg, bool error = false);

private:
    Config                  config_;
    Config                  running_;
    std::string             path_;
    ModelCache&             models_;
    FlowEngine&             flows_;
    SendFn                  send_fn_;
    GraphWindow*            graph_window_ = nullptr;
    bool                    restart_requested_ = false;
    std::string             saved_yaml_;           // content of the last save/load
    std::function<std::optional<DebugSession>()> debug_snapshot_;
    double                  autosave_due_ = -1.0;
    double                  last_autosave_time_ = -1.0;

    std::string              status_;
    bool                     status_error_ = false;
    std::vector<std::string> warnings_;
};
