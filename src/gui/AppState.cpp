#include "AppState.hpp"
#include "config/RecentConfigs.hpp"
#include "flow/Legacy.hpp"

#include <ctime>

#include "GraphPreset.hpp"
#include "GraphWindow.hpp"

#include <format>

AppState::AppState(Config config, std::string path, ModelCache& models, FlowEngine& flows)
    : config_(config), running_(std::move(config)), path_(std::move(path))
    , models_(models), flows_(flows)
{}

void AppState::set_status(std::string msg, bool error)
{
    status_       = std::move(msg);
    status_error_ = error;
}

Config AppState::collect() const
{
    Config out = config_;
    for (auto& iface : out.interfaces)
        iface.actions.clear();

    // Actions and guards are flows now.
    if (graph_window_)
        out.graphs = graph_window_->export_config();

    out.guards.clear();

    out.flows.clear();
    for (const auto& f : flows_.snapshot())
        out.flows.push_back(f.spec);
    if (flow_override_) flow_override_(out.flows);
    if (xcp_snapshot_) out.xcp = xcp_snapshot_();
    return out;
}

void AppState::record_debug_session()
{
    if (!debug_snapshot_) return;
    auto session = debug_snapshot_();
    if (!session) return;

    auto& hist = config_.debug_history;
    // newest first, one entry per log file; unchanged sessions keep their timestamp
    for (auto it = hist.begin(); it != hist.end(); ++it) {
        if (it->log != session->log) continue;
        if (it->same_content(*session)) session->saved = it->saved;
        hist.erase(it);
        break;
    }
    if (session->saved.empty()) {
        const std::time_t now = std::time(nullptr);
        std::tm           tm{};
        localtime_r(&now, &tm);
        char buf[32];
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
        session->saved = buf;
    }
    hist.insert(hist.begin(), std::move(*session));
    constexpr std::size_t MAX_HISTORY = 20;
    if (hist.size() > MAX_HISTORY) hist.resize(MAX_HISTORY);
}

bool AppState::save(const std::string& path)
{
    try {
        record_debug_session();
        unapplied_note_.clear();
        const Config c = collect();
        save_config(c, path);
        saved_yaml_ = config_to_yaml(c);
        path_ = path;
        remember_config(path);
        set_status(std::format("Saved {}", path) + (unapplied_note_.empty() ? "" : " - " + unapplied_note_));
        return true;
    } catch (const std::exception& e) {
        set_status(std::format("Save failed: {}", e.what()), true);
        return false;
    }
}

bool AppState::load(const std::string& path)
{
    Config loaded;
    try {
        loaded = load_config(path);
    } catch (const std::exception& e) {
        set_status(std::format("Load failed: {}", e.what()), true);
        return false;
    }
    const auto converted = migrate_legacy(loaded);

    config_ = loaded;
    path_   = path;
    remember_config(path);
    warnings_.clear();

    flows_.clear();
    flows_.add_all(config_.flows);

    if (graph_window_) {
        graph_window_->clear();
        apply_graph_config(config_, *graph_window_,
                           [this](const std::string& iface) { return models_.model_for(iface); });
    }

    saved_yaml_.clear();   // actions/graphs are created asynchronously; first autosave compares fresh
    if (!converted.empty())
        set_status(std::format("Loaded {} - {} action(s)/guard(s) converted to flows", path, converted.size()));
    else if (channels_changed())
        set_status(std::format("Loaded {} - channel settings differ, restart to apply them", path));
    else
        set_status(std::format("Loaded {}", path));
    return true;
}

void AppState::autosave_tick(double now)
{
    const int every = config_.gui.autosave_s;
    if (every <= 0) {
        autosave_due_ = -1.0;
        return;
    }
    if (autosave_due_ < 0.0 || autosave_due_ > now + every)
        autosave_due_ = now + every;
    if (now < autosave_due_)
        return;
    autosave_due_ = now + every;

    try {
        record_debug_session();
        const Config      c    = collect();
        const std::string yaml = config_to_yaml(c);
        if (yaml == saved_yaml_)
            return;                       // nothing changed
        save_config(c, path_);
        saved_yaml_         = yaml;
        last_autosave_time_ = now;
        set_status(std::format("Auto-saved {}", path_));
    } catch (const std::exception& e) {
        set_status(std::format("Auto-save failed: {}", e.what()), true);
    }
}
