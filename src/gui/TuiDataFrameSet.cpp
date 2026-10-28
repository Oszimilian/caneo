#include "TuiDataFrameSet.hpp"
#include "LogViewer.hpp"
#include "config/AppConfig.hpp"
#include "guard/GuardEngine.hpp"
#include "logger/ILogControl.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <thread>

using namespace ftxui;

// ─── Trace search helper ───────────────────────────────────────────────────

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

static bool trace_matches(uint32_t id, const CanFrame& frame, const std::string& query) {
    const std::string q = to_lower(query);
    auto ci_contains = [&q](const std::string& hay) {
        return to_lower(hay).find(q) != std::string::npos;
    };
    if (ci_contains(std::format("0x{:03X}", id))) return true;
    if (ci_contains(frame.msg_name())) return true;
    for (const auto& sig : frame.decoded()) {
        if (ci_contains(sig.name)) return true;
        if (ci_contains(sig.unit)) return true;
    }
    return false;
}

// ─── Construction ──────────────────────────────────────────────────────────

TuiDataFrameSet::TuiDataFrameSet(const std::vector<InterfaceConfig>& iface_configs,
                                 ActionHandler&                      action_handler,
                                 PlaybackController*                 playback_ctrl)
    : action_handler_(action_handler), playback_ctrl_(playback_ctrl)
{
    for (const auto& cfg : iface_configs) {
        interfaces_.push_back(cfg.name);
        if (!cfg.dbc.empty()) {
            try {
                send_models_.emplace(cfg.name, std::make_unique<SendModel>(cfg.dbc));
            } catch (...) {}
        }
    }
}

// ─── Data update (asio thread) ─────────────────────────────────────────────

void TuiDataFrameSet::update(const CanFrame& frame) {
    {
        std::lock_guard lock(mutex_);
        const std::string& iface = frame.header().interface;
        if (!sets_.contains(iface))
            sets_.emplace(iface, DataFrameSet{iface});
        sets_.at(iface).update(frame);
        if (!graphs_.empty()) {
            const double t = now_s();
            for (const auto& sig : frame.decoded())
                record(iface, frame.msg_name(), sig.name, sig.value, t);
        }
    }
    screen_.PostEvent(Event::Custom);
}

// ─── Helpers ───────────────────────────────────────────────────────────────

const std::string& TuiDataFrameSet::selected_send_iface() const {
    static const std::string empty;
    if (interfaces_.empty()) return empty;
    return interfaces_[std::min(sub_tab_send_, static_cast<int>(interfaces_.size()) - 1)];
}

SendModel* TuiDataFrameSet::selected_send_model() {
    auto it = send_models_.find(selected_send_iface());
    return it != send_models_.end() ? it->second.get() : nullptr;
}

const SendModel* TuiDataFrameSet::selected_send_model() const {
    auto it = send_models_.find(selected_send_iface());
    return it != send_models_.end() ? it->second.get() : nullptr;
}

void TuiDataFrameSet::create_single_action() {
    const SendModel* model = selected_send_model();
    if (!model || model->messages().empty()) return;
    const int msg_idx = std::min(send_msg_cursor_, static_cast<int>(model->messages().size()) - 1);
    if (msg_idx < 0) return;
    const SendMessage& msg = model->messages()[msg_idx];

    action_handler_.add_action(std::make_unique<SingleAction>(
        action_handler_.io_ref(),
        selected_send_iface(),
        msg.id,
        msg.name,
        model->encode(static_cast<std::size_t>(msg_idx)),
        action_handler_.send_fn_ref()));
}

void TuiDataFrameSet::create_periodic_action(std::chrono::milliseconds period) {
    const SendModel* model = selected_send_model();
    if (!model || model->messages().empty()) return;
    const int msg_idx = std::min(send_msg_cursor_, static_cast<int>(model->messages().size()) - 1);
    if (msg_idx < 0) return;
    const SendMessage& msg = model->messages()[msg_idx];

    action_handler_.add_action(std::make_unique<PeriodicAction>(
        action_handler_.io_ref(),
        selected_send_iface(),
        msg.id,
        msg.name,
        model->encode(static_cast<std::size_t>(msg_idx)),
        action_handler_.send_fn_ref(),
        period));
}

void TuiDataFrameSet::create_sin_action(double amplitude,
                                         std::chrono::milliseconds sin_period,
                                         double offset) {
    SendModel* model = selected_send_model();
    if (!model || model->messages().empty()) return;
    const int msg_idx = std::min(send_msg_cursor_, static_cast<int>(model->messages().size()) - 1);
    if (msg_idx < 0) return;
    const SendMessage& msg = model->messages()[msg_idx];

    const int sig_count = static_cast<int>(msg.signals.size());
    if (sig_count == 0) return;
    const std::size_t sig_idx = static_cast<std::size_t>(std::min(send_sig_cursor_, sig_count - 1));
    const std::size_t mi      = static_cast<std::size_t>(msg_idx);

    // Encode callback runs on the asio thread; captures raw model ptr (owned by send_models_)
    SinPeriodicAction::EncodeWithValueFn encode_fn =
        [model, mi, sig_idx](double v) {
            model->set_value(mi, sig_idx, v);
            return model->encode(mi);
        };

    action_handler_.add_action(std::make_unique<SinPeriodicAction>(
        action_handler_.io_ref(),
        selected_send_iface(),
        msg.id,
        msg.name,
        action_handler_.send_fn_ref(),
        std::chrono::milliseconds(periodic_interval_ms_),
        std::move(encode_fn),
        amplitude,
        sin_period,
        offset));
}

// ─── Event loop ────────────────────────────────────────────────────────────

void TuiDataFrameSet::set_status_indicator(std::string label, std::function<bool()> connected_fn) {
    status_label_        = std::move(label);
    status_connected_fn_ = std::move(connected_fn);
}

void TuiDataFrameSet::enable_model_tab() {
    model_tab_enabled_ = true;
}

void TuiDataFrameSet::push_model_output(const std::string& name, double value) {
    {
        std::lock_guard lock(mutex_);
        model_outputs_[name] = value;
        record(MODEL_IFACE, "", name, value, now_s());
    }
    screen_.PostEvent(Event::Custom);
}

std::vector<TuiDataFrameSet::Tab> TuiDataFrameSet::tabs() const {
    std::vector<Tab> t = {Tab::Trace, Tab::Send, Tab::Actions};
    if (guard_engine_)      t.push_back(Tab::Guards);
    t.push_back(Tab::Graphs);
    if (model_tab_enabled_) t.push_back(Tab::Model);
    if (playback_ctrl_)     t.push_back(Tab::Playback);
    return t;
}

void TuiDataFrameSet::set_log_control(ILogControl* log, const Config* config) {
    log_    = log;
    config_ = config;
}

void TuiDataFrameSet::set_message(std::string msg, bool error) {
    message_       = std::move(msg);
    message_error_ = error;
    message_time_  = std::chrono::steady_clock::now();
}

void TuiDataFrameSet::toggle_log() {
    namespace fs = std::filesystem;
    if (!log_)
        return;
    if (log_->is_logging()) {
        log_->stop();
        set_message("Log saved: " + log_files(log_->last_path()));
    } else if (!log_->supports_path()) {
        log_->start();
        set_message("Log started (remote)");
    } else {
        const fs::path dir = config_ ? config_->log_dir().value_or(fs::current_path())
                                     : fs::current_path();
        fs::path name = AppConfig::log_file_name();
        if (config_ && config_->logging.format == "blf") name.replace_extension(".blf");
        log_path_buf_ = (dir / name).string();
        log_prompt_   = true;
    }
}

void TuiDataFrameSet::open_viewer() {
    if (!log_)
        return;
    const std::string file = log_->is_logging() ? log_->current_path() : log_->last_path();
    std::string cmd = config_ ? config_->log_viewer : std::string{};
    if (cmd.empty())
        cmd = log_viewer::detect_plotjuggler().value_or("");
    if (cmd.empty())
        set_message("No log viewer configured (log_viewer: in the config)", true);
    else if (file.empty())
        set_message("No log file yet - start one with [l]", true);
    else if (const auto err = log_viewer::launch(cmd, file))
        set_message(*err, true);
    else
        set_message("Opened " + file);
}

// ─── Mouse ─────────────────────────────────────────────────────────────────

Element TuiDataFrameSet::clickable(Element e, std::function<void()> fn, bool keep_modes) {
    ClickTarget& t = click_targets_.emplace_back(ClickTarget{{}, std::move(fn), keep_modes});
    t.box.x_min = t.box.y_min = 1;   // empty until the element is drawn
    t.box.x_max = t.box.y_max = 0;
    return e | reflect(t.box);
}

Element TuiDataFrameSet::button(const std::string& label, std::function<void()> fn,
                                bool highlight, bool keep_modes) {
    Element e = text("[" + label + "]");
    e = highlight ? e | inverted : e | color(Color::Blue);
    return clickable(e, std::move(fn), keep_modes);
}

Element TuiDataFrameSet::tab_bar(const std::vector<std::string>& labels, int selected, bool focused,
                                 std::function<void(int)> on_click) {
    Elements tabs;
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        Element label = text(" " + labels[i] + " ");
        if (i == selected)
            label = label | inverted | (focused ? bold : dim);
        tabs.push_back(clickable(std::move(label), [on_click, i] { on_click(i); }));
        if (i + 1 < static_cast<int>(labels.size()))
            tabs.push_back(text("│"));
    }
    return hbox(std::move(tabs));
}

bool TuiDataFrameSet::handle_mouse(const Mouse& mouse) {
    if (mouse.button == Mouse::WheelUp)   { scroll(-1); return true; }
    if (mouse.button == Mouse::WheelDown) { scroll(+1); return true; }
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed)
        return true;

    // innermost target = smallest box containing the click
    const ClickTarget* hit = nullptr;
    auto area = [](const Box& b) { return (b.x_max - b.x_min + 1) * (b.y_max - b.y_min + 1); };
    for (const auto& t : click_targets_)
        if (t.box.Contain(mouse.x, mouse.y) && (!hit || area(t.box) < area(hit->box)))
            hit = &t;
    if (!hit)
        return true;
    if (!hit->keep_modes)
        cancel_modes();
    const auto fn = hit->fn;   // the handler may change what gets rendered
    fn();
    return true;
}

void TuiDataFrameSet::cancel_modes() {
    send_editing_    = false;
    actions_editing_ = false;
    log_prompt_      = false;
    send_edit_buf_.clear();
    actions_edit_buf_.clear();
    periodic_step_ = PeriodicStep::None;
    periodic_interval_buf_.clear();
    periodic_interval_ms_ = 0;
    periodic_type_cursor_ = 0;
    sin_amplitude_buf_.clear();
    sin_period_buf_.clear();
    sin_offset_buf_.clear();
}

void TuiDataFrameSet::select_tab(Tab tab) {
    main_tab_  = tab;
    nav_level_ = 1;
}

// Mouse wheel: moves the cursor of the list under the current view.
void TuiDataFrameSet::scroll(int delta) {
    auto step = [delta](int& cursor, int count) {
        cursor = std::clamp(cursor + delta, 0, std::max(0, count - 1));
    };
    if (nav_level_ == 0) nav_level_ = 1;

    switch (main_tab_) {
        case Tab::Trace: {
            std::lock_guard lock(mutex_);
            const DataFrameSet* set = active_trace_set();
            if (!set) return;
            if (nav_level_ == 2) {
                const auto order = trace_order(*set);
                if (trace_cursor_ < static_cast<int>(order.size()))
                    step(trace_sig_cursor_,
                         static_cast<int>(set->frames().at(order[trace_cursor_]).decoded().size()));
            } else {
                step(trace_cursor_, static_cast<int>(set->frames().size()));
            }
            break;
        }
        case Tab::Send: {
            const SendModel* model = selected_send_model();
            if (!model || model->messages().empty()) return;
            if (nav_level_ == 3) {
                const int mi = std::min(send_msg_cursor_, static_cast<int>(model->messages().size()) - 1);
                step(send_sig_cursor_, static_cast<int>(model->messages()[mi].signals.size()) + 2);
            } else {
                nav_level_ = 2;
                step(send_msg_cursor_, static_cast<int>(model->messages().size()));
            }
            break;
        }
        case Tab::Actions:
            if (nav_level_ == 1)
                step(actions_cursor_, static_cast<int>(action_handler_.snapshot().size()));
            break;
        case Tab::Guards:
            if (guard_engine_ && nav_level_ == 1)
                step(guards_cursor_, static_cast<int>(guard_engine_->snapshot().size()));
            break;
        case Tab::Playback:
            if (!playback_ctrl_) return;
            if (nav_level_ == 2) {
                const auto snap = playback_ctrl_->snapshot();
                if (playback_cursor_ < static_cast<int>(snap.size()))
                    step(playback_msg_cursor_,
                         static_cast<int>(playback_ctrl_->message_snapshot(snap[playback_cursor_].first).size()) + 1);
            } else {
                step(playback_cursor_, static_cast<int>(playback_ctrl_->snapshot().size()));
            }
            break;
        case Tab::Graphs: {
            std::lock_guard lock(mutex_);
            step(graph_cursor_, static_cast<int>(current_graph().series.size()));
            break;
        }
        case Tab::Model:
            break;
    }
}

// ─── Graphs ────────────────────────────────────────────────────────────────

double TuiDataFrameSet::now_s() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
}

void TuiDataFrameSet::set_graphs(const std::vector<GraphConfig>& graphs) {
    std::lock_guard lock(mutex_);
    graphs_.clear();
    for (const auto& gc : graphs) {
        GraphView g{.name = gc.name.empty() ? "Graph" : gc.name, .window_s = gc.window_s, .auto_y = gc.auto_y};
        for (const auto& sc : gc.signals) {
            std::string iface = sc.interface;
            if (sc.msg.empty() && iface != GUARD_IFACE) iface = MODEL_IFACE;
            g.series.push_back({.iface = iface, .msg = sc.msg, .signal = sc.signal,
                                .y_min = sc.y_min, .y_max = sc.y_max});
        }
        graphs_.push_back(std::move(g));
    }
    if (graphs_.empty())
        graphs_.push_back({.name = "Graph 1"});
}

TuiDataFrameSet::GraphView& TuiDataFrameSet::current_graph() {
    if (graphs_.empty())
        graphs_.push_back({.name = "Graph 1"});
    graph_tab_ = std::clamp(graph_tab_, 0, static_cast<int>(graphs_.size()) - 1);
    return graphs_[static_cast<std::size_t>(graph_tab_)];
}

void TuiDataFrameSet::record(const std::string& iface, const std::string& msg, const std::string& signal,
                             double value, double t) {
    constexpr double      KEEP_S     = 600.0;    // history kept per series
    constexpr std::size_t MAX_POINTS = 100'000;
    for (auto& g : graphs_)
        for (auto& s : g.series) {
            if (s.signal != signal || s.msg != msg || (!s.iface.empty() && s.iface != iface))
                continue;
            s.pts.emplace_back(t, value);
            while (!s.pts.empty() && (s.pts.front().first < t - KEEP_S || s.pts.size() > MAX_POINTS))
                s.pts.pop_front();
        }
}

bool TuiDataFrameSet::graphed(const std::string& iface, const std::string& msg,
                              const std::string& signal) const {
    if (graphs_.empty()) return false;
    const auto& g = graphs_[static_cast<std::size_t>(std::clamp(graph_tab_, 0, static_cast<int>(graphs_.size()) - 1))];
    return std::ranges::any_of(g.series, [&](const GraphSeries& s) {
        return s.signal == signal && s.msg == msg && (s.iface.empty() || s.iface == iface);
    });
}

void TuiDataFrameSet::toggle_graph_signal(const std::string& iface, const std::string& msg,
                                          const std::string& signal) {
    std::lock_guard lock(mutex_);
    GraphView& g = current_graph();
    const auto it = std::ranges::find_if(g.series, [&](const GraphSeries& s) {
        return s.signal == signal && s.msg == msg && (s.iface.empty() || s.iface == iface);
    });
    const std::string label = msg.empty() ? signal : msg + "." + signal;
    if (it != g.series.end()) {
        g.series.erase(it);
        set_message("Removed " + label + " from " + g.name);
    } else {
        g.series.push_back({.iface = iface, .msg = msg, .signal = signal});
        set_message("Added " + label + " to " + g.name + "  (Graphs [v])");
    }
}

void TuiDataFrameSet::push_guard_output(const std::string& guard, const std::string& output, double value) {
    {
        std::lock_guard lock(mutex_);
        record(GUARD_IFACE, "", guard + "/" + output, value, now_s());
    }
    screen_.PostEvent(Event::Custom);
}

void TuiDataFrameSet::graph_guard_outputs(const GuardInfo& guard) {
    const auto v = guard.state ? guard.state->values() : GuardState::Values{};
    if (v.outputs.empty()) {
        set_message("Guard " + guard.spec.name + " has no outputs yet", true);
        return;
    }
    std::lock_guard lock(mutex_);
    GraphView& g = current_graph();
    int added = 0;
    for (const auto& [out, _] : v.outputs) {
        const std::string name = guard.spec.name + "/" + out;
        if (!graphed(GUARD_IFACE, "", name)) {
            g.series.push_back({.iface = GUARD_IFACE, .signal = name});
            ++added;
        }
    }
    set_message(std::format("{} output(s) of {} added to {}  (Graphs [v])", added, guard.spec.name, g.name));
}

void TuiDataFrameSet::trace_toggle_selected_signal() {
    std::string iface, msg, signal;
    {
        std::lock_guard lock(mutex_);
        const DataFrameSet* set = active_trace_set();
        if (!set) return;
        const auto order = trace_order(*set);
        if (trace_cursor_ >= static_cast<int>(order.size())) return;
        const CanFrame& f = set->frames().at(order[trace_cursor_]);
        if (trace_sig_cursor_ >= static_cast<int>(f.decoded().size())) return;
        iface  = f.header().interface;
        msg    = f.msg_name();
        signal = f.decoded()[static_cast<std::size_t>(trace_sig_cursor_)].name;
    }
    toggle_graph_signal(iface, msg, signal);
}

// ─── Trace helpers ─────────────────────────────────────────────────────────

const DataFrameSet* TuiDataFrameSet::active_trace_set() const {
    if (sets_.empty()) return nullptr;
    const int idx = std::min(sub_tab_trace_, static_cast<int>(sets_.size()) - 1);
    return &std::next(sets_.begin(), idx)->second;
}

std::vector<uint32_t> TuiDataFrameSet::trace_order(const DataFrameSet& set) const {
    const bool filter = trace_searching_ && !trace_search_buf_.empty();
    std::vector<uint32_t> matched, rest;
    for (const auto& [id, frame] : set.frames())
        (!filter || trace_matches(id, frame, trace_search_buf_) ? matched : rest).push_back(id);
    matched.insert(matched.end(), rest.begin(), rest.end());
    return matched;
}

// "x.mcap" -> "x.mcap + .blf" when both formats are written
std::string TuiDataFrameSet::log_files(const std::string& path) const {
    if (config_ && config_->logging.format == "both")
        return path + " + .blf";
    return path;
}

// 'l' opens a path prompt (or stops a running log), 'o' opens the last log in
// the configured viewer. Returns true if the event was consumed.
bool TuiDataFrameSet::handle_log_event(const Event& event) {
    namespace fs = std::filesystem;
    if (!log_)
        return false;

    if (log_prompt_) {
        if (event == Event::Escape) {
            log_prompt_ = false;
            return true;
        }
        if (event == Event::Return) {
            log_prompt_ = false;
            try {
                log_->start(log_path_buf_);
                if (log_->is_logging())
                    set_message("Logging to " + log_files(log_->current_path()));
                else
                    set_message("Log could not be started", true);
            } catch (const std::exception& e) {
                set_message(std::string("Log start failed: ") + e.what(), true);
            }
            return true;
        }
        if (event == Event::Backspace) {
            if (!log_path_buf_.empty()) log_path_buf_.pop_back();
            return true;
        }
        if (event.is_character()) {
            log_path_buf_ += event.character();
            return true;
        }
        return true;   // swallow navigation while the prompt is open
    }

    if (event == Event::Character('l')) { toggle_log();  return true; }
    if (event == Event::Character('o')) { open_viewer(); return true; }
    return false;
}

void TuiDataFrameSet::run() {
    action_handler_.set_notify([this] { screen_.PostEvent(Event::Custom); });

    auto renderer = Renderer([this] { return render(); });

    // Redraw once a second without CAN traffic (REC timer, messages).
    std::atomic<bool> ticking{true};
    std::thread ticker([this, &ticking] {
        while (ticking) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            screen_.PostEvent(Event::Custom);
        }
    });

    key_handler_ = [this](Event event) -> bool {

        // ── Mouse: clicks on registered targets, wheel scrolls ────────────
        if (event.is_mouse())
            return handle_mouse(event.mouse());

        // ── Log path prompt (modal) ───────────────────────────────────────
        if (log_prompt_)
            return handle_log_event(event);

        // ── Actions signal edit ───────────────────────────────────────────
        if (actions_editing_) {
            if (event == Event::Return || event == Event::ArrowLeft) {
                const auto snap = action_handler_.snapshot();
                if (actions_cursor_ < static_cast<int>(snap.size()) && !actions_edit_buf_.empty()) {
                    const ActionInfo& info = snap[actions_cursor_];
                    // Sin action: update sin parameter directly
                    if (info.type_name == "Sin") {
                        try {
                            action_handler_.update_sin_param(
                                static_cast<std::size_t>(actions_cursor_),
                                actions_sig_cursor_,
                                std::stod(actions_edit_buf_));
                        } catch (...) {}
                        actions_editing_ = false;
                        actions_edit_buf_.clear();
                        return true;
                    }
                    // Regular action: commit value into SendModel, re-encode + update payload
                    auto it = send_models_.find(info.interface);
                    if (it != send_models_.end()) {
                        SendModel& model = *it->second;
                        const auto& msgs = model.messages();
                        // find message by id
                        for (int mi = 0; mi < static_cast<int>(msgs.size()); ++mi) {
                            if (msgs[mi].id == info.msg_id) {
                                const int sig_count = static_cast<int>(msgs[mi].signals.size());
                                const int sig_idx   = std::min(actions_sig_cursor_, sig_count - 1);
                                if (sig_idx >= 0) {
                                    try {
                                        model.set_value(static_cast<std::size_t>(mi),
                                                        static_cast<std::size_t>(sig_idx),
                                                        std::stod(actions_edit_buf_));
                                        action_handler_.update_payload(
                                            static_cast<std::size_t>(actions_cursor_),
                                            model.encode(static_cast<std::size_t>(mi)));
                                    } catch (...) {}
                                }
                                break;
                            }
                        }
                    }
                }
                actions_editing_ = false;
                actions_edit_buf_.clear();
                return true;
            }
            if (event == Event::Escape) {
                actions_editing_ = false;
                actions_edit_buf_.clear();
                return true;
            }
            if (event == Event::Backspace) {
                if (!actions_edit_buf_.empty()) actions_edit_buf_.pop_back();
                return true;
            }
            if (event.is_character()) {
                const char c = event.character()[0];
                if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-')
                    actions_edit_buf_ += c;
                return true;
            }
            return false;
        }

        // ── Periodic / Sin action state machine ───────────────────────────
        if (periodic_step_ != PeriodicStep::None) {
            using PS = PeriodicStep;

            auto reset_periodic = [this] {
                periodic_step_        = PS::None;
                periodic_interval_buf_.clear();
                periodic_interval_ms_ = 0;
                periodic_type_cursor_ = 0;
                sin_amplitude_buf_.clear();
                sin_period_buf_.clear();
                sin_offset_buf_.clear();
            };

            if (event == Event::Escape) { reset_periodic(); return true; }

            // Step: entering interval (ms)
            if (periodic_step_ == PS::Period) {
                if (event == Event::Return || event == Event::ArrowRight) {
                    if (!periodic_interval_buf_.empty()) {
                        try {
                            periodic_interval_ms_ = std::stol(periodic_interval_buf_);
                            if (periodic_interval_ms_ > 0)
                                periodic_step_ = PS::TypeSelect;
                        } catch (...) {}
                    }
                    return true;
                }
                if (event == Event::Backspace) {
                    if (!periodic_interval_buf_.empty()) periodic_interval_buf_.pop_back();
                    return true;
                }
                if (event.is_character() && std::isdigit(static_cast<unsigned char>(event.character()[0]))) {
                    periodic_interval_buf_ += event.character()[0];
                    return true;
                }
                return false;
            }

            // Step: Constant vs Sin
            if (periodic_step_ == PS::TypeSelect) {
                if (event == Event::ArrowLeft || event == Event::ArrowRight) {
                    periodic_type_cursor_ = 1 - periodic_type_cursor_;
                    return true;
                }
                if (event == Event::Return) {
                    if (periodic_type_cursor_ == 0) {
                        create_periodic_action(std::chrono::milliseconds(periodic_interval_ms_));
                        reset_periodic();
                    } else {
                        periodic_step_ = PS::SinAmplitude;
                    }
                    return true;
                }
                if (event == Event::ArrowUp) { periodic_step_ = PS::Period; return true; }
                return false;
            }

            // Helper for numeric field input (digits + optional '.' and leading '-')
            auto handle_num = [&](std::string& buf, bool allow_decimal) -> bool {
                if (event == Event::Backspace) {
                    if (!buf.empty()) buf.pop_back();
                    return true;
                }
                if (event.is_character()) {
                    const char c = event.character()[0];
                    if (std::isdigit(static_cast<unsigned char>(c))) { buf += c; return true; }
                    if (allow_decimal && c == '.' && buf.find('.') == std::string::npos) { buf += c; return true; }
                    if (allow_decimal && c == '-' && buf.empty()) { buf += c; return true; }
                    return true;  // swallow other characters
                }
                return false;
            };

            if (periodic_step_ == PS::SinAmplitude) {
                if (event == Event::Return || event == Event::ArrowDown) { periodic_step_ = PS::SinPeriod;   return true; }
                if (event == Event::ArrowUp)                              { periodic_step_ = PS::TypeSelect; return true; }
                return handle_num(sin_amplitude_buf_, true);
            }
            if (periodic_step_ == PS::SinPeriod) {
                if (event == Event::Return || event == Event::ArrowDown) { periodic_step_ = PS::SinOffset;    return true; }
                if (event == Event::ArrowUp)                              { periodic_step_ = PS::SinAmplitude; return true; }
                return handle_num(sin_period_buf_, false);
            }
            if (periodic_step_ == PS::SinOffset) {
                if (event == Event::Return || event == Event::ArrowDown) { periodic_step_ = PS::SinReady;  return true; }
                if (event == Event::ArrowUp)                              { periodic_step_ = PS::SinPeriod; return true; }
                return handle_num(sin_offset_buf_, true);
            }
            if (periodic_step_ == PS::SinReady) {
                if (event == Event::Return || event == Event::ArrowRight) {
                    try {
                        const double amp    = sin_amplitude_buf_.empty() ? 0.0  : std::stod(sin_amplitude_buf_);
                        const long   sp_ms  = sin_period_buf_.empty()    ? 1000 : std::stol(sin_period_buf_);
                        const double offset = sin_offset_buf_.empty()    ? 0.0  : std::stod(sin_offset_buf_);
                        if (sp_ms > 0)
                            create_sin_action(amp, std::chrono::milliseconds(sp_ms), offset);
                    } catch (...) {}
                    reset_periodic();
                    return true;
                }
                if (event == Event::ArrowUp) { periodic_step_ = PS::SinOffset; return true; }
                return false;
            }
            return false;
        }

        // ── Signal value edit ─────────────────────────────────────────────
        if (send_editing_) {
            if (event == Event::Return || event == Event::ArrowLeft) {
                SendModel* model = selected_send_model();
                if (model && !send_edit_buf_.empty()) {
                    try {
                        const double val  = std::stod(send_edit_buf_);
                        const int msg_idx = std::min(send_msg_cursor_,
                                                     static_cast<int>(model->messages().size()) - 1);
                        if (msg_idx >= 0) {
                            const int sig_idx = std::min(
                                send_sig_cursor_,
                                static_cast<int>(model->messages()[msg_idx].signals.size()) - 1);
                            if (sig_idx >= 0)
                                model->set_value(static_cast<std::size_t>(msg_idx),
                                                 static_cast<std::size_t>(sig_idx), val);
                        }
                    } catch (...) {}
                }
                send_editing_ = false;
                send_edit_buf_.clear();
                return true;
            }
            if (event == Event::Escape) {
                send_editing_ = false;
                send_edit_buf_.clear();
                return true;
            }
            if (event == Event::Backspace) {
                if (!send_edit_buf_.empty()) send_edit_buf_.pop_back();
                return true;
            }
            if (event.is_character()) {
                const char c = event.character()[0];
                if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-')
                    send_edit_buf_ += c;
                return true;
            }
            return false;
        }

        // ── Trace search input ────────────────────────────────────────────
        if (trace_searching_) {
            if (event == Event::Escape) {
                trace_searching_ = false;
                trace_search_buf_.clear();
                return true;
            }
            if (event == Event::Backspace) {
                if (!trace_search_buf_.empty()) trace_search_buf_.pop_back();
                return true;
            }
            if (event.is_character()) {
                trace_search_buf_ += event.character();
                return true;
            }
            // arrows / Enter navigate the (filtered) trace below
        }

        // ── Quit ──────────────────────────────────────────────────────────
        if (event == Event::Character('q') || event == Event::Escape) {
            screen_.ExitLoopClosure()();
            return true;
        }

        if (handle_log_event(event))
            return true;

        // ── Tab shortcuts ─────────────────────────────────────────────────
        if (event == Event::Character('t')) { main_tab_ = Tab::Trace;   nav_level_ = 1; return true; }
        if (event == Event::Character('s')) { main_tab_ = Tab::Send;    nav_level_ = 1; return true; }
        if (event == Event::Character('a')) { main_tab_ = Tab::Actions; nav_level_ = 1; return true; }
        if (event == Event::Character('g') && guard_engine_) {
            main_tab_ = Tab::Guards; nav_level_ = 1; return true;
        }
        if (event == Event::Character('m') && model_tab_enabled_) {
            main_tab_ = Tab::Model; nav_level_ = 1; return true;
        }
        if (event == Event::Character('p') && playback_ctrl_) {
            main_tab_ = Tab::Playback; nav_level_ = 1; return true;
        }
        if (event == Event::Character('v')) { select_tab(Tab::Graphs); return true; }
        if (event == Event::Character('f') && main_tab_ == Tab::Trace) {
            trace_searching_ = true;
            return true;
        }

        // ── Trace: Enter = into the signals of the frame, Enter/+ = graph ──
        if (main_tab_ == Tab::Trace && nav_level_ == 1 && event == Event::Return) {
            std::lock_guard lock(mutex_);
            const DataFrameSet* set = active_trace_set();
            if (set) {
                const auto order = trace_order(*set);
                if (trace_cursor_ < static_cast<int>(order.size()) &&
                    !set->frames().at(order[trace_cursor_]).decoded().empty()) {
                    trace_sig_cursor_ = 0;
                    nav_level_        = 2;
                }
            }
            return true;
        }
        if (main_tab_ == Tab::Trace && nav_level_ == 2) {
            if (event == Event::ArrowLeft) { nav_level_ = 1; return true; }
            if (event == Event::ArrowUp) {
                if (trace_sig_cursor_ > 0) --trace_sig_cursor_;
                else nav_level_ = 1;
                return true;
            }
            if (event == Event::ArrowDown) { scroll(+1); return true; }
            if (event == Event::Return || event == Event::Character('+') || event == Event::Character(' ')) {
                trace_toggle_selected_signal();
                return true;
            }
            return false;
        }

        // ── Guards: + = outputs into the current graph ────────────────────
        if (main_tab_ == Tab::Guards && nav_level_ == 1 && event == Event::Character('+') && guard_engine_) {
            const auto snap = guard_engine_->snapshot();
            if (guards_cursor_ < static_cast<int>(snap.size()))
                graph_guard_outputs(snap[guards_cursor_]);
            return true;
        }

        // ── Graphs ────────────────────────────────────────────────────────
        if (main_tab_ == Tab::Graphs && nav_level_ >= 1) {
            std::lock_guard lock(mutex_);
            GraphView& g = current_graph();
            const int count = static_cast<int>(g.series.size());
            if (event == Event::ArrowLeft || event == Event::ArrowRight) {
                const int n = static_cast<int>(graphs_.size());
                graph_tab_    = (graph_tab_ + (event == Event::ArrowRight ? 1 : n - 1)) % n;
                graph_cursor_ = 0;
                return true;
            }
            if (event == Event::ArrowUp) {
                if (graph_cursor_ > 0) --graph_cursor_;
                else nav_level_ = 0;
                return true;
            }
            if (event == Event::ArrowDown) {
                if (graph_cursor_ + 1 < count) ++graph_cursor_;
                return true;
            }
            if (event == Event::Character(' ')) {
                graph_paused_  = !graph_paused_;
                graph_pause_t_ = now_s();
                return true;
            }
            if ((event == Event::Delete || event == Event::Backspace) && graph_cursor_ < count) {
                g.series.erase(g.series.begin() + graph_cursor_);
                graph_cursor_ = std::max(0, std::min(graph_cursor_, count - 2));
                return true;
            }
            if (event == Event::Character('+')) { g.window_s = std::max(1.0, g.window_s / 2); return true; }
            if (event == Event::Character('-')) { g.window_s = std::min(600.0, g.window_s * 2); return true; }
            if (event == Event::Character('y')) { g.auto_y = !g.auto_y; return true; }
            if (event == Event::Character('c')) { for (auto& s : g.series) s.pts.clear(); return true; }
            if (event == Event::Character('n')) {
                graphs_.push_back({.name = std::format("Graph {}", graphs_.size() + 1)});
                graph_tab_    = static_cast<int>(graphs_.size()) - 1;
                graph_cursor_ = 0;
                return true;
            }
            return false;
        }

        // ── Signal list (nav_level_ == 3) ─────────────────────────────────
        if (nav_level_ == 3) {
            if (event == Event::ArrowLeft) {
                nav_level_ = 2;
                return true;
            }
            if (event == Event::ArrowRight || event == Event::Return) {
                const SendModel* model = selected_send_model();
                if (model) {
                    const int msg_idx = std::min(send_msg_cursor_,
                                                 static_cast<int>(model->messages().size()) - 1);
                    if (msg_idx >= 0) {
                        const int sig_count =
                            static_cast<int>(model->messages()[msg_idx].signals.size());
                        if (send_sig_cursor_ < sig_count) {
                            // Enter signal edit mode
                            const auto& sig = model->messages()[msg_idx].signals[send_sig_cursor_];
                            send_edit_buf_ = std::format("{:.6g}", sig.value);
                            send_editing_  = true;
                        } else if (send_sig_cursor_ == sig_count) {
                            // Single Action button
                            create_single_action();
                        } else {
                            // Periodic Action button
                            periodic_step_ = PeriodicStep::Period;
                            periodic_interval_buf_.clear();
                        }
                    }
                }
                return true;
            }
            if (event == Event::ArrowUp) {
                if (send_sig_cursor_ > 0) --send_sig_cursor_;
                return true;
            }
            if (event == Event::ArrowDown) {
                const SendModel* model = selected_send_model();
                // max = sig_count + 1 (0=signals, sig_count=Single, sig_count+1=Periodic)
                int max_idx = 1;
                if (model) {
                    const int msg_idx = std::min(send_msg_cursor_,
                                                 static_cast<int>(model->messages().size()) - 1);
                    if (msg_idx >= 0)
                        max_idx = static_cast<int>(model->messages()[msg_idx].signals.size()) + 1;
                }
                if (send_sig_cursor_ < max_idx) ++send_sig_cursor_;
                return true;
            }
            if (event == Event::Character('1')) {
                create_single_action();
                return true;
            }
            if (event == Event::Character('2')) {
                const SendModel* model = selected_send_model();
                if (model && !model->messages().empty()) {
                    const int msg_idx = std::min(send_msg_cursor_,
                                                 static_cast<int>(model->messages().size()) - 1);
                    if (msg_idx >= 0)
                        send_sig_cursor_ = static_cast<int>(
                            model->messages()[msg_idx].signals.size()) + 1;
                }
                periodic_step_ = PeriodicStep::Period;
                periodic_interval_buf_.clear();
                return true;
            }
            return false;
        }

        // ── nav_level_ == 2: playback message filter ──────────────────────
        if (nav_level_ == 2 && main_tab_ == Tab::Playback && playback_ctrl_) {
            if (event == Event::ArrowLeft) {
                nav_level_ = 1;
                return true;
            }
            if (event == Event::ArrowUp) {
                if (playback_msg_cursor_ > 0) --playback_msg_cursor_;
                return true;
            }
            if (event == Event::ArrowDown) {
                const auto snap = playback_ctrl_->snapshot();
                if (playback_cursor_ < static_cast<int>(snap.size())) {
                    const auto msgs = playback_ctrl_->message_snapshot(
                        snap[playback_cursor_].first);
                    // 0 = All, 1..N = individual
                    if (playback_msg_cursor_ < static_cast<int>(msgs.size()))
                        ++playback_msg_cursor_;
                }
                return true;
            }
            if (event == Event::Return) {
                const auto snap = playback_ctrl_->snapshot();
                if (playback_cursor_ < static_cast<int>(snap.size()))
                    playback_ctrl_->toggle_message(snap[playback_cursor_].first,
                                                   playback_msg_cursor_);
                return true;
            }
            return false;
        }

        // ── nav_level_ == 2: guard log ────────────────────────────────────
        if (nav_level_ == 2 && main_tab_ == Tab::Guards) {
            if (event == Event::ArrowLeft || event == Event::Return) {
                nav_level_ = 1;
                return true;
            }
            return false;
        }

        // ── nav_level_ == 2: message list (Send) or action signal view (Actions) ──
        if (nav_level_ == 2) {
            if (main_tab_ == Tab::Actions) {
                // Actions tab: signal edit view
                if (event == Event::ArrowLeft) {
                    nav_level_ = 1;
                    return true;
                }
                if (event == Event::Character(' ')) {
                    const auto snap = action_handler_.snapshot();
                    if (actions_cursor_ < static_cast<int>(snap.size()))
                        action_handler_.toggle_pause(snap[actions_cursor_].idx);
                    return true;
                }
                if (event == Event::ArrowRight || event == Event::Return) {
                    const auto snap = action_handler_.snapshot();
                    if (actions_cursor_ < static_cast<int>(snap.size())) {
                        const ActionInfo& info = snap[actions_cursor_];
                        if (info.type_name == "Sin") {
                            // Prefill with current sin parameter value
                            switch (actions_sig_cursor_) {
                                case 0: actions_edit_buf_ = std::format("{:.6g}", info.sin_amplitude); break;
                                case 1: actions_edit_buf_ = std::to_string(info.sin_period_ms);        break;
                                case 2: actions_edit_buf_ = std::format("{:.6g}", info.sin_offset);    break;
                            }
                            actions_editing_ = true;
                        } else {
                            auto it = send_models_.find(info.interface);
                            if (it != send_models_.end()) {
                                const auto& msgs = it->second->messages();
                                for (const auto& msg : msgs) {
                                    if (msg.id == info.msg_id && !msg.signals.empty()) {
                                        const auto& sig = msg.signals[std::min(
                                            actions_sig_cursor_,
                                            static_cast<int>(msg.signals.size()) - 1)];
                                        actions_edit_buf_ = std::format("{:.6g}", sig.value);
                                        actions_editing_  = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    return true;
                }
                if (event == Event::ArrowUp) {
                    if (actions_sig_cursor_ > 0) --actions_sig_cursor_;
                    return true;
                }
                if (event == Event::ArrowDown) {
                    const auto snap = action_handler_.snapshot();
                    if (actions_cursor_ < static_cast<int>(snap.size())) {
                        const ActionInfo& info = snap[actions_cursor_];
                        if (info.type_name == "Sin") {
                            if (actions_sig_cursor_ < 2) ++actions_sig_cursor_;
                        } else {
                            auto it = send_models_.find(info.interface);
                            if (it != send_models_.end()) {
                                for (const auto& msg : it->second->messages()) {
                                    if (msg.id == info.msg_id) {
                                        const int max = static_cast<int>(msg.signals.size()) - 1;
                                        if (actions_sig_cursor_ < max) ++actions_sig_cursor_;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    return true;
                }
                return false;
            }

            // Send tab: message list
            if (event == Event::ArrowLeft) {
                nav_level_ = 1;
                return true;
            }
            if (event == Event::ArrowRight || event == Event::Return) {
                const SendModel* model = selected_send_model();
                if (model && !model->messages().empty()) {
                    send_sig_cursor_ = 0;
                    nav_level_       = 3;
                }
                return true;
            }
            if (event == Event::ArrowUp) {
                if (send_msg_cursor_ > 0) --send_msg_cursor_;
                return true;
            }
            if (event == Event::ArrowDown) {
                const SendModel* model = selected_send_model();
                if (model) {
                    const int count = static_cast<int>(model->messages().size());
                    if (send_msg_cursor_ + 1 < count) ++send_msg_cursor_;
                }
                return true;
            }
            return false;
        }

        // ── Sub-tabs / Action list (nav_level_ == 1) ──────────────────────
        if (nav_level_ == 1) {
            // Playback tab: interface list
            if (main_tab_ == Tab::Playback && playback_ctrl_) {
                if (event == Event::ArrowUp) {
                    if (playback_cursor_ > 0) --playback_cursor_;
                    else nav_level_ = 0;
                    return true;
                }
                if (event == Event::ArrowDown) {
                    const auto snap = playback_ctrl_->snapshot();
                    if (playback_cursor_ + 1 < static_cast<int>(snap.size()))
                        ++playback_cursor_;
                    return true;
                }
                if (event == Event::Return) {
                    const auto snap = playback_ctrl_->snapshot();
                    if (playback_cursor_ < static_cast<int>(snap.size()))
                        playback_ctrl_->toggle(snap[playback_cursor_].first);
                    return true;
                }
                if (event == Event::ArrowRight || event == Event::Return) {
                    const auto snap = playback_ctrl_->snapshot();
                    if (playback_cursor_ < static_cast<int>(snap.size())) {
                        playback_msg_cursor_ = 0;
                        nav_level_ = 2;
                    }
                    return true;
                }
                return false;
            }

            // Guards tab: list, Space = enable/disable, → = output log
            if (main_tab_ == Tab::Guards && guard_engine_) {
                const auto snap = guard_engine_->snapshot();
                if (event == Event::ArrowUp) {
                    if (guards_cursor_ > 0) --guards_cursor_;
                    else nav_level_ = 0;
                    return true;
                }
                if (event == Event::ArrowDown) {
                    if (guards_cursor_ + 1 < static_cast<int>(snap.size())) ++guards_cursor_;
                    return true;
                }
                if (event == Event::Character(' ')) {
                    if (guards_cursor_ < static_cast<int>(snap.size()))
                        guard_engine_->set_enabled(snap[guards_cursor_].idx,
                                                   !snap[guards_cursor_].spec.enabled);
                    return true;
                }
                if (event == Event::ArrowRight || event == Event::Return) {
                    if (guards_cursor_ < static_cast<int>(snap.size())) nav_level_ = 2;
                    return true;
                }
                return false;
            }

            // Actions tab: navigate action list
            if (main_tab_ == Tab::Actions) {
                if (event == Event::ArrowUp) {
                    if (actions_cursor_ > 0)
                        --actions_cursor_;
                    else
                        nav_level_ = 0;
                    return true;
                }
                if (event == Event::ArrowDown) {
                    const auto snap = action_handler_.snapshot();
                    if (actions_cursor_ + 1 < static_cast<int>(snap.size()))
                        ++actions_cursor_;
                    return true;
                }
                if (event == Event::ArrowRight || event == Event::Return) {
                    const auto snap = action_handler_.snapshot();
                    if (actions_cursor_ < static_cast<int>(snap.size())) {
                        actions_sig_cursor_ = 0;
                        nav_level_          = 2;
                    }
                    return true;
                }
                if (event == Event::Character(' ')) {
                    const auto snap = action_handler_.snapshot();
                    if (actions_cursor_ < static_cast<int>(snap.size()))
                        action_handler_.toggle_pause(snap[actions_cursor_].idx);
                    return true;
                }
                if (event == Event::Delete || event == Event::Backspace) {
                    const auto snap = action_handler_.snapshot();
                    if (actions_cursor_ < static_cast<int>(snap.size())) {
                        action_handler_.remove_action(snap[actions_cursor_].idx);
                        if (actions_cursor_ > 0) --actions_cursor_;
                    }
                    return true;
                }
                return false;
            }

            // Trace / Send: sub-tab navigation
            if (event == Event::ArrowUp) {
                if (main_tab_ == Tab::Trace && trace_cursor_ > 0) {
                    --trace_cursor_;
                } else {
                    nav_level_ = 0;
                }
                return true;
            }
            if (event == Event::ArrowDown && main_tab_ == Tab::Trace) {
                std::lock_guard lock(mutex_);
                const auto& active = sets_;
                // count visible frames in the active interface
                std::vector<std::string> names;
                for (const auto& [n, _] : active) names.push_back(n);
                if (!names.empty()) {
                    const int idx = sub_tab_trace_ < static_cast<int>(names.size())
                                        ? sub_tab_trace_ : static_cast<int>(names.size()) - 1;
                    const int count = static_cast<int>(active.at(names[idx]).frames().size());
                    if (trace_cursor_ + 1 < count) ++trace_cursor_;
                }
                return true;
            }
            if (event == Event::ArrowDown && main_tab_ == Tab::Send) {
                const SendModel* model = selected_send_model();
                if (model && !model->messages().empty()) {
                    send_msg_cursor_ = 0;
                    nav_level_       = 2;
                }
                return true;
            }
            if (event == Event::ArrowRight) {
                if (main_tab_ == Tab::Trace) {
                    std::lock_guard lock(mutex_);
                    if (!sets_.empty()) {
                        sub_tab_trace_ = (sub_tab_trace_ + 1) % static_cast<int>(sets_.size());
                        trace_cursor_ = 0;
                    }
                } else {
                    if (!interfaces_.empty())
                        sub_tab_send_ = (sub_tab_send_ + 1) % static_cast<int>(interfaces_.size());
                }
                return true;
            }
            if (event == Event::ArrowLeft) {
                if (main_tab_ == Tab::Trace) {
                    std::lock_guard lock(mutex_);
                    if (!sets_.empty()) {
                        sub_tab_trace_ = (sub_tab_trace_ + static_cast<int>(sets_.size()) - 1)
                                         % static_cast<int>(sets_.size());
                        trace_cursor_ = 0;
                    }
                } else {
                    if (!interfaces_.empty())
                        sub_tab_send_ = (sub_tab_send_ + static_cast<int>(interfaces_.size()) - 1)
                                        % static_cast<int>(interfaces_.size());
                }
                return true;
            }
            return false;
        }

        // ── Main tabs (nav_level_ == 0) ───────────────────────────────────
        if (event == Event::ArrowDown) {
            nav_level_ = 1;
            return true;
        }
        if (event == Event::ArrowRight || event == Event::ArrowLeft) {
            const auto t     = tabs();
            const int  count = static_cast<int>(t.size());
            const int  cur   = static_cast<int>(std::find(t.begin(), t.end(), main_tab_) - t.begin());
            const int  step  = event == Event::ArrowRight ? 1 : count - 1;
            main_tab_ = t[static_cast<std::size_t>((cur + step) % count)];
            return true;
        }
        return false;
    };
    renderer |= CatchEvent([this](Event event) { return key_handler_(event); });

    screen_.Loop(renderer);
    ticking = false;
    ticker.join();
}

// ─── Rendering helpers ─────────────────────────────────────────────────────

static std::string fmt_value(double v) {
    return std::format("{:.6g}", v);
}

static Color series_color(int i) {
    static const Color colors[] = {Color::Blue,    Color::Red,        Color::Green,
                                   Color::Magenta, Color::DarkOrange, Color::Cyan,
                                   Color::Purple,  Color::GrayDark};
    return colors[static_cast<std::size_t>(i) % std::size(colors)];
}

static std::string series_label(const std::string& iface, const std::string& msg, const std::string& signal) {
    if (msg.empty())
        return std::string(iface == "__guards__" ? "guard " : "model ") + signal;
    return msg + "." + signal;
}

// Index of the series in g, or -1.
static int series_index(const std::vector<std::pair<std::string, std::string>>& keys,
                        const std::string& msg, const std::string& signal) {
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (keys[i].first == msg && keys[i].second == signal)
            return static_cast<int>(i);
    return -1;
}

void TuiDataFrameSet::emit(const Event& event) {
    if (key_handler_)
        key_handler_(event);
}

// ─── Trace ─────────────────────────────────────────────────────────────────

Element TuiDataFrameSet::render_trace() {
    std::vector<std::string> iface_names;
    for (const auto& [name, _] : sets_)
        iface_names.push_back(name);

    Element bar = hbox({
        tab_bar(iface_names, sub_tab_trace_, nav_level_ == 1,
                [this](int i) { sub_tab_trace_ = i; trace_cursor_ = 0; nav_level_ = 1; }),
        filler(),
        nav_level_ == 2 ? button("Enter → Graph", [this] { trace_toggle_selected_signal(); })
                        : button("Enter Signale", [this] { emit(Event::Return); }),
        text(" "),
        trace_searching_ ? button("f Suche ✕", [this] { trace_searching_ = false; trace_search_buf_.clear(); })
                         : button("f Suche", [this] { trace_searching_ = true; }),
    });

    const DataFrameSet* set = active_trace_set();
    if (!set)
        return vbox({bar, separator(), text("Waiting for frames...") | dim | center});

    // series of the current graph, to mark graphed signals
    std::vector<std::pair<std::string, std::string>> graph_keys;
    if (!graphs_.empty())
        for (const auto& s : current_graph().series)
            graph_keys.emplace_back(s.msg, s.signal);

    const auto        order  = trace_order(*set);
    const bool        filter = trace_searching_ && !trace_search_buf_.empty();
    const std::string iface  = iface_names[static_cast<std::size_t>(std::min(sub_tab_trace_, static_cast<int>(iface_names.size()) - 1))];
    Elements          matched_rows, unmatched_rows;

    for (int r = 0; r < static_cast<int>(order.size()); ++r) {
        const uint32_t  id    = order[static_cast<std::size_t>(r)];
        const CanFrame& frame = set->frames().at(id);
        const bool      sel   = r == trace_cursor_ && nav_level_ >= 1;

        std::string delta_str = " | Δ ---    ";
        if (const auto d = set->delta(id))
            delta_str = std::format(" | Δ {:6.1f}ms", std::chrono::duration<double, std::milli>(*d).count());

        std::string hex_str;
        for (const uint8_t byte : frame.payload())
            hex_str += std::format("{:02X} ", byte);
        if (!hex_str.empty()) hex_str.pop_back();

        const auto&        sigs = frame.decoded();
        const std::string& name = frame.msg_name();
        Element head = hbox(text(std::format("0x{:03X}", id)) | bold,
                            name.empty() ? text("") : text("  " + name) | bold,
                            text(" | DLC:" + std::to_string(frame.header().dlc)),
                            text(delta_str) | dim,
                            sigs.empty() ? text(" | " + hex_str) | dim : text(""),
                            filler());
        if (sel && nav_level_ == 1) head = head | inverted | focus;
        Elements block = {clickable(head, [this, r] { trace_cursor_ = r; nav_level_ = 1; })};

        for (int j = 0; j < static_cast<int>(sigs.size()); ++j) {
            const auto& sig = sigs[static_cast<std::size_t>(j)];
            const int   ci  = series_index(graph_keys, name, sig.name);
            // checkbox: click toggles the signal in the current graph
            Element box = clickable(ci >= 0 ? text(" ■ ") | color(series_color(ci)) | bold : text(" □ ") | dim,
                                    [this, r, j] {
                                        trace_cursor_ = r; trace_sig_cursor_ = j; nav_level_ = 2;
                                        trace_toggle_selected_signal();
                                    });
            Element line = hbox({text(" "), box,
                                 text(sig.name + ": ") ,
                                 text(fmt_value(sig.value)) | bold,
                                 text(" " + sig.unit) | dim,
                                 filler()});
            const bool sig_sel = sel && nav_level_ == 2 && j == trace_sig_cursor_;
            if (sig_sel) line = line | inverted | focus;
            block.push_back(clickable(line, [this, r, j] {
                const bool again = nav_level_ == 2 && trace_cursor_ == r && trace_sig_cursor_ == j;
                trace_cursor_ = r; trace_sig_cursor_ = j; nav_level_ = 2;
                if (again) trace_toggle_selected_signal();   // second click = graph
            }));
        }
        Element e = vbox(std::move(block));
        if (!filter || trace_matches(id, frame, trace_search_buf_)) matched_rows.push_back(std::move(e));
        else                                                         unmatched_rows.push_back(std::move(e));
    }

    Elements top = {bar, separator()};
    if (trace_searching_) {
        top.push_back(hbox(text("Suche: [") | dim, text(trace_search_buf_), text("_") | inverted, text("]") | dim,
                           filler(), text("ID, Name, Signal, Einheit") | dim));
        top.push_back(separator());
    }

    if (!filter) {
        if (matched_rows.empty()) matched_rows.push_back(text("(no frames)") | dim);
        top.push_back(vbox(std::move(matched_rows)) | vscroll_indicator | yframe | flex);
        return vbox(std::move(top));
    }
    if (matched_rows.empty())   matched_rows.push_back(text("(keine Treffer)") | dim);
    if (unmatched_rows.empty()) unmatched_rows.push_back(text("(keine weiteren Frames)") | dim);
    top.push_back(vbox(std::move(matched_rows)) | vscroll_indicator | yframe | flex);
    top.push_back(separator());
    top.push_back(vbox(std::move(unmatched_rows)) | vscroll_indicator | yframe);
    return vbox(std::move(top));
}

// ─── Send: message list ────────────────────────────────────────────────────

Element TuiDataFrameSet::render_msg_list(const SendModel& model) {
    const auto& msgs = model.messages();
    Elements    rows;
    for (int i = 0; i < static_cast<int>(msgs.size()); ++i) {
        const bool sel = (i == send_msg_cursor_);
        Element    row = hbox(text(std::format("{}0x{:03X}  {}", sel ? "▶ " : "  ", msgs[i].id, msgs[i].name)),
                              filler());
        if (sel && nav_level_ == 2) row = row | bold | inverted;
        else if (sel)               row = row | bold;
        if (sel)                    row = row | focus;
        rows.push_back(clickable(row, [this, i] {
            if (nav_level_ == 2 && send_msg_cursor_ == i) { send_sig_cursor_ = 0; nav_level_ = 3; }
            else                                          { send_msg_cursor_ = i; nav_level_ = 2; }
        }));
    }
    if (rows.empty())
        rows.push_back(text("(no messages in DBC)") | dim);
    return vbox({
        vbox(std::move(rows)) | vscroll_indicator | yframe | flex,
        separator(),
        hbox(text("Klick/→ öffnet die Message") | dim, filler()),
    });
}

// ─── Send: signal list ─────────────────────────────────────────────────────

Element TuiDataFrameSet::render_sig_list(const SendModel& model) {
    const auto& msgs    = model.messages();
    const int   msg_idx = std::min(send_msg_cursor_, static_cast<int>(msgs.size()) - 1);
    if (msg_idx < 0) return text("(no message selected)") | dim;

    const SendMessage& msg = msgs[msg_idx];
    Element header = hbox({button("◀ zurück", [this] { nav_level_ = 2; }), text(" "),
                           text(std::format("0x{:03X}  {}", msg.id, msg.name)) | bold});

    Elements sig_rows;
    for (int i = 0; i < static_cast<int>(msg.signals.size()); ++i) {
        const SendSignal& sig     = msg.signals[i];
        const bool        is_cur  = (i == send_sig_cursor_);
        const bool        editing = is_cur && send_editing_;

        Element val_elem = editing
            ? hbox(hbox(text("["), text(send_edit_buf_), text("_]")) | inverted, text(" "),
                   button("OK", [this] { emit(Event::Return); }, false, true))
            : text(fmt_value(sig.value));

        Element row = hbox(text(is_cur ? "▶ " : "  "),
                           text(sig.name + ": ") | (is_cur ? bold : nothing),
                           val_elem,
                           text(" " + sig.unit) | dim,
                           filler());
        if (is_cur && !editing && nav_level_ == 3) row = row | inverted;
        if (is_cur)                                  row = row | focus;
        sig_rows.push_back(clickable(row, [this, i] {
            if (nav_level_ == 3 && send_sig_cursor_ == i) emit(Event::Return);   // edit
            else { send_sig_cursor_ = i; nav_level_ = 3; }
        }));
    }
    if (sig_rows.empty())
        sig_rows.push_back(text("(no signals)") | dim);

    // ── Action buttons below the signal list ──────────────────────────────
    const int  sig_count    = static_cast<int>(msg.signals.size());
    const bool cur_single   = (send_sig_cursor_ == sig_count);
    const bool cur_periodic = (send_sig_cursor_ == sig_count + 1);

    sig_rows.push_back(text("")); // spacing

    Element single_row = hbox(text(cur_single ? "▶ " : "  "), text("[ Single Action ]") | color(Color::Blue));
    if (cur_single && nav_level_ == 3) single_row = single_row | bold | inverted;
    if (cur_single)                    single_row = single_row | focus;
    sig_rows.push_back(clickable(single_row, [this, sig_count] {
        send_sig_cursor_ = sig_count; nav_level_ = 3;
        create_single_action();
        set_message("Single action sent");
    }));

    using PS = PeriodicStep;
    const bool in_periodic = (periodic_step_ != PS::None);

    Element periodic_row;
    if (cur_periodic && periodic_step_ == PS::Period) {
        periodic_row = hbox(hbox(text("▶ [ Periodic Action ]   Interval: ["), text(periodic_interval_buf_),
                                 text("_] ms")) | bold | inverted,
                            text(" "), button("OK", [this] { emit(Event::Return); }, false, true));
    } else if (cur_periodic && in_periodic) {
        periodic_row = hbox(text("▶ [ Periodic Action ]   Interval: "),
                            text(std::to_string(periodic_interval_ms_) + " ms")) | bold | inverted;
    } else {
        periodic_row = hbox(text(cur_periodic ? "▶ " : "  "), text("[ Periodic Action ]") | color(Color::Blue));
        if (cur_periodic && nav_level_ == 3) periodic_row = periodic_row | bold | inverted;
        periodic_row = clickable(periodic_row, [this, sig_count] {
            send_sig_cursor_ = sig_count + 1; nav_level_ = 3;
            periodic_step_   = PeriodicStep::Period;
            periodic_interval_buf_.clear();
        });
    }
    if (cur_periodic) periodic_row = periodic_row | focus;
    sig_rows.push_back(std::move(periodic_row));

    // TypeSelect
    if (cur_periodic && periodic_step_ == PS::TypeSelect) {
        const bool on_const = (periodic_type_cursor_ == 0);
        Element const_elem = text("[Constant]");
        const_elem = on_const ? const_elem | bold | inverted : const_elem | color(Color::Blue);
        Element sin_elem = text("[Sin]");
        sin_elem = !on_const ? sin_elem | bold | inverted : sin_elem | color(Color::Blue);
        sig_rows.push_back(hbox(
            text("    "),
            text(on_const ? "▶ " : "  "),
            clickable(std::move(const_elem), [this] { periodic_type_cursor_ = 0; emit(Event::Return); }, true),
            text("   "),
            text(!on_const ? "▶ " : "  "),
            clickable(std::move(sin_elem), [this] { periodic_type_cursor_ = 1; emit(Event::Return); }, true)));
    }

    // Sin parameter fields (shown once we're past TypeSelect)
    if (cur_periodic && static_cast<int>(periodic_step_) >= static_cast<int>(PS::SinAmplitude)) {
        auto field = [&](const std::string& label, const std::string& buf, const std::string& unit, PS step) {
            const bool active = (periodic_step_ == step);
            Element val = active ? hbox(text("["), text(buf), text("_]")) : text(buf.empty() ? "-" : buf);
            Element row = hbox(text("    "), text(label) | dim, val, text(" " + unit) | dim);
            sig_rows.push_back(clickable(active ? (row | bold | inverted) : row,
                                         [this, step] { periodic_step_ = step; }, true));
        };
        field("Amplitude: ", sin_amplitude_buf_, "",   PS::SinAmplitude);
        field("Period:    ", sin_period_buf_,    "ms", PS::SinPeriod);
        field("Offset:    ", sin_offset_buf_,    "",   PS::SinOffset);

        Element start = hbox(text("    "), text("[ Start ]"));
        start = periodic_step_ == PS::SinReady ? (start | bold | inverted) : (start | color(Color::Blue));
        sig_rows.push_back(clickable(start, [this] { periodic_step_ = PeriodicStep::SinReady; emit(Event::Return); }, true));
    }

    // ── Raw frame preview ─────────────────────────────────────────────────
    const std::vector<uint8_t> raw = model.encode(static_cast<std::size_t>(msg_idx));
    std::string hex_str;
    for (const uint8_t b : raw) hex_str += std::format("{:02X} ", b);
    if (!hex_str.empty()) hex_str.pop_back();

    return vbox({
        header,
        separator(),
        vbox(std::move(sig_rows)) | vscroll_indicator | yframe | flex,
        separator(),
        hbox(text("Raw: ") | dim, text(hex_str) | bold),
        separator(),
        hbox(button("1 Single Action", [this, sig_count] {
                 send_sig_cursor_ = sig_count; nav_level_ = 3; emit(Event::Character('1'));
                 set_message("Single action sent");
             }),
             text(" "),
             button("2 Periodic Action", [this] { nav_level_ = 3; emit(Event::Character('2')); }),
             text(" "),
             button("Esc abbrechen", [this] { cancel_modes(); }),
             filler(),
             text("Klick auf Signal = auswählen, nochmal = Wert ändern") | dim),
    });
}

// ─── Send ──────────────────────────────────────────────────────────────────

Element TuiDataFrameSet::render_send() {
    const int count    = static_cast<int>(interfaces_.size());
    const int idx      = count > 0 ? std::min(sub_tab_send_, count - 1) : 0;
    Element   sub_tabs = tab_bar(interfaces_, idx, nav_level_ == 1, [this](int i) {
        sub_tab_send_ = i; send_msg_cursor_ = 0; nav_level_ = 2;
    });

    Element content;
    const SendModel* model = selected_send_model();
    if (!model)
        content = text("No DBC configured for this interface.") | dim | center;
    else if (nav_level_ >= 3)
        content = render_sig_list(*model);
    else
        content = render_msg_list(*model);

    return vbox({sub_tabs, separator(), content | flex});
}

// ─── Action signal edit view ───────────────────────────────────────────────

Element TuiDataFrameSet::render_action_signals(const ActionInfo& info) {
    if (info.type_name == "Lua")
        return render_action_log(info);

    Element back = button("◀ zurück", [this] { emit(Event::ArrowLeft); });

    auto it = send_models_.find(info.interface);
    if (it == send_models_.end())
        return vbox({back, text("Kein SendModel für dieses Interface.") | dim | center});

    const SendModel& model = *it->second;
    const auto& msgs = model.messages();

    // find message by id
    const SendMessage* msg_ptr = nullptr;
    int msg_idx = -1;
    for (int i = 0; i < static_cast<int>(msgs.size()); ++i) {
        if (msgs[i].id == info.msg_id) { msg_ptr = &msgs[i]; msg_idx = i; break; }
    }
    if (!msg_ptr || msg_idx < 0)
        return vbox({back, text("Message nicht in DBC gefunden.") | dim | center});

    const SendMessage& msg = *msg_ptr;
    Element header = hbox({back, text(" "),
                           text(std::format("[{}] 0x{:03X}  {}  ({}, {}ms interval){}",
                                            info.interface, msg.id, msg.name, info.type_name,
                                            info.period.count(), info.paused ? "  [paused]" : "")) | bold,
                           filler(),
                           button(info.paused ? "Space Start" : "Space Pause",
                                  [this] { emit(Event::Character(' ')); })});

    auto value_row = [&](int i, const std::string& label, const std::string& value, const std::string& unit) {
        const bool is_cur  = (i == actions_sig_cursor_);
        const bool editing = is_cur && actions_editing_;
        Element val = editing
            ? hbox(hbox(text("["), text(actions_edit_buf_), text("_]")) | inverted, text(" "),
                   button("OK", [this] { emit(Event::Return); }, false, true))
            : text(value);
        Element row = hbox(text(is_cur ? "▶ " : "  "),
                           text(label) | (is_cur ? bold : nothing),
                           val,
                           text(unit.empty() ? "" : " " + unit) | dim,
                           filler());
        if (is_cur && !editing && nav_level_ == 2) row = row | inverted;
        if (is_cur) row = row | focus;
        return clickable(row, [this, i] {
            if (actions_sig_cursor_ == i) emit(Event::Return);   // edit
            else actions_sig_cursor_ = i;
        });
    };

    // Sin action: show sin parameters instead of signal list
    if (info.type_name == "Sin") {
        Elements rows;
        rows.push_back(value_row(0, "Amplitude: ", fmt_value(info.sin_amplitude), ""));
        rows.push_back(value_row(1, "Period:    ", std::to_string(info.sin_period_ms), "ms"));
        rows.push_back(value_row(2, "Offset:    ", fmt_value(info.sin_offset), ""));
        return vbox({header, separator(), vbox(std::move(rows)) | yframe | flex});
    }

    Elements sig_rows;
    for (int i = 0; i < static_cast<int>(msg.signals.size()); ++i) {
        const SendSignal& sig = msg.signals[i];
        sig_rows.push_back(value_row(i, sig.name + ": ", fmt_value(sig.value), sig.unit));
    }
    if (sig_rows.empty())
        sig_rows.push_back(text("(keine Signale)") | dim);

    const std::vector<uint8_t> raw = model.encode(static_cast<std::size_t>(msg_idx));
    std::string hex_str;
    for (const uint8_t b : raw) hex_str += std::format("{:02X} ", b);
    if (!hex_str.empty()) hex_str.pop_back();

    return vbox({
        header,
        separator(),
        vbox(std::move(sig_rows)) | vscroll_indicator | yframe | flex,
        separator(),
        hbox(text("Raw: ") | dim, text(hex_str) | bold),
    });
}

// ─── Lua action: script + print() output ───────────────────────────────────

static Element render_log_lines(const ActionLog* log) {
    Elements rows;
    if (log) {
        if (const std::string err = log->error(); !err.empty())
            rows.push_back(paragraph("Error: " + err) | color(Color::Red));
        for (const auto& line : log->lines())
            rows.push_back(text(line));
    }
    if (rows.empty())
        rows.push_back(text("(no output)") | dim);
    // newest line at the bottom, scrolled into view
    rows.back() = rows.back() | focus;
    return vbox(std::move(rows)) | vscroll_indicator | yframe | flex;
}

Element TuiDataFrameSet::render_action_log(const ActionInfo& info) {
    Element header = hbox({
        button("◀ zurück", [this] { emit(Event::ArrowLeft); }), text(" "),
        text(std::format("[{}] {}  (Lua, {}ms){}", info.interface,
                         info.spec.name.empty() ? info.msg_name : info.spec.name,
                         info.period.count(), info.paused ? "  [paused]" : "")) | bold,
        filler(),
        button(info.paused ? "Space Start" : "Space Pause", [this] { emit(Event::Character(' ')); }),
    });
    return vbox({
        header,
        text("Script: " + info.spec.script) | dim,
        separator(),
        render_log_lines(info.log.get()),
    });
}

// ─── Guards ────────────────────────────────────────────────────────────────

Element TuiDataFrameSet::render_guards() {
    const auto snap = guard_engine_ ? guard_engine_->snapshot() : std::vector<GuardInfo>{};
    if (snap.empty()) {
        std::string where = config_ && !config_->guards.empty() ? "" : " (die geladene Config hat keine guards:)";
        return vbox({
            text("Keine Guards definiert" + where + ".") | dim | center,
            separator(),
            text("Guards werden in der Config (guards:) oder in der GUI angelegt.") | dim | center,
        });
    }

    const int cursor = std::min(guards_cursor_, static_cast<int>(snap.size()) - 1);

    // Output log of the selected guard
    if (nav_level_ == 2) {
        const GuardInfo& g = snap[cursor];
        return vbox({
            hbox({button("◀ zurück", [this] { emit(Event::ArrowLeft); }), text(" "),
                  text(std::format("{}  [{}] {}.{}", g.spec.name, g.spec.interface, g.spec.msg, g.spec.signal)) | bold}),
            text("Script: " + g.spec.script) | dim,
            separator(),
            render_log_lines(g.state ? &g.state->log : nullptr),
        });
    }

    // column widths from the content
    auto signal_of = [](const GuardInfo& g) {
        return std::format("{} {}.{}", g.spec.interface, g.spec.msg, g.spec.signal);
    };
    int name_w = 6, sig_w = 8;
    for (const auto& g : snap) {
        name_w = std::max(name_w, static_cast<int>(g.spec.name.size()) + 2);
        sig_w  = std::max(sig_w,  static_cast<int>(signal_of(g).size()) + 2);
    }

    Elements rows;
    for (int i = 0; i < static_cast<int>(snap.size()); ++i) {
        const GuardInfo& g   = snap[i];
        const bool       sel = (i == cursor);
        const auto       v   = g.state ? g.state->values() : GuardState::Values{};
        const bool       err = !g.loaded || (g.state && !g.state->log.error().empty());

        Element status = err             ? text("● ") | color(Color::Red)
                       : g.spec.enabled  ? text("● ") | color(Color::Green)
                                         : text("○ ") | dim;

        std::string outputs;
        for (const auto& [name, value] : v.outputs)
            outputs += std::format("{}={:.6g}  ", name, value);
        if (outputs.empty()) outputs = "-";

        Element row = hbox({
            text(sel ? "▶ " : "  "),
            status,
            text(g.spec.name) | bold | size(WIDTH, EQUAL, name_w),
            text(signal_of(g)) | size(WIDTH, EQUAL, sig_w),
            text(v.input ? fmt_value(*v.input) : "-") | size(WIDTH, EQUAL, 12),
            text(std::format("{}", v.count)) | dim | size(WIDTH, EQUAL, 9),
            text(outputs) | flex_shrink | flex,   // absorbs overflow
            button(g.spec.enabled ? "an " : "aus", [this, i] { guards_cursor_ = i; nav_level_ = 1; emit(Event::Character(' ')); },
                   g.spec.enabled),
            text(" "),
            button("+Graph", [this, i] { guards_cursor_ = i; nav_level_ = 1; emit(Event::Character('+')); }),
        });
        if (sel && nav_level_ == 1) row = row | inverted;
        if (sel)                    row = row | focus;
        rows.push_back(clickable(row, [this, i] {
            if (nav_level_ == 1 && guards_cursor_ == i) emit(Event::ArrowRight);   // open log
            else { guards_cursor_ = i; nav_level_ = 1; }
        }));
    }

    return vbox({
        hbox({text("    "),
              text("Name")      | size(WIDTH, EQUAL, name_w),
              text("Signal")    | size(WIDTH, EQUAL, sig_w),
              text("Input")     | size(WIDTH, EQUAL, 12),
              text("Count")     | size(WIDTH, EQUAL, 9),
              text("Outputs") | flex_shrink}) | dim,
        separator(),
        vbox(std::move(rows)) | vscroll_indicator | yframe | flex,
        separator(),
        hbox(button("Space an/aus", [this] { nav_level_ = 1; emit(Event::Character(' ')); }), text(" "),
             button("→ Ausgaben / Fehler", [this] { nav_level_ = 1; emit(Event::ArrowRight); }), text(" "),
             button("+ in Graph", [this] { nav_level_ = 1; emit(Event::Character('+')); }),
             filler()),
    });
}

// ─── Actions ───────────────────────────────────────────────────────────────

Element TuiDataFrameSet::render_actions() {
    const auto snap = action_handler_.snapshot();

    // Signal edit view for a specific action
    if (nav_level_ == 2 && actions_cursor_ < static_cast<int>(snap.size()))
        return render_action_signals(snap[actions_cursor_]);

    if (snap.empty()) {
        return vbox({
            text("Keine Actions definiert.") | dim | center,
            separator(),
            text("Im Send-Tab eine Message öffnen und Single/Periodic Action wählen.") | dim | center,
        });
    }

    Elements rows;
    for (int i = 0; i < static_cast<int>(snap.size()); ++i) {
        const auto& a   = snap[i];
        const bool  sel = (i == actions_cursor_);

        std::string period_str = a.is_periodic ? std::format("{:>6}ms", a.period.count()) : "      -  ";

        std::string last_str = "never";
        if (a.ever_sent) {
            // Frozen while paused
            const auto ref = a.paused && a.paused_at > a.last_sent ? a.paused_at
                                                                   : std::chrono::steady_clock::now();
            const auto ago = std::chrono::duration_cast<std::chrono::milliseconds>(ref - a.last_sent);
            if (ago.count() < 1000)
                last_str = std::format("{:4}ms ago", ago.count());
            else
                last_str = std::format("{:.1f}s ago", static_cast<double>(ago.count()) / 1000.0);
        }

        const std::string label = std::format(
            "{}0x{:03X}  {:<20}  {:<8}  {}  {}  {:<10}",
            sel ? "▶ " : "  ", a.msg_id,
            a.type_name == "Lua" && !a.spec.name.empty() ? a.spec.name : a.msg_name,
            a.type_name, period_str, a.interface, last_str);

        Element row = hbox({
            text(label),
            text(a.paused ? " [paused]" : "         ") | dim,
            filler(),
            button(a.paused ? "Start" : "Pause", [this, i] { actions_cursor_ = i; nav_level_ = 1; emit(Event::Character(' ')); },
                   !a.paused),
            text(" "),
            button("✕", [this, i] { actions_cursor_ = i; nav_level_ = 1; emit(Event::Delete); }),
        });
        if (sel && nav_level_ == 1) row = row | bold | inverted;
        else if (sel)               row = row | bold;
        if (sel)                    row = row | focus;
        rows.push_back(clickable(row, [this, i] {
            if (nav_level_ == 1 && actions_cursor_ == i) emit(Event::ArrowRight);   // open
            else { actions_cursor_ = i; nav_level_ = 1; }
        }));
    }

    return vbox({
        text(std::format("  {:<5}  {:<20}  {:<8}  {:<9}  {:<6}  {}",
                         "ID", "Name", "Typ", "Periode", "Iface", "Letzter Send")) | dim,
        separator(),
        vbox(std::move(rows)) | vscroll_indicator | yframe | flex,
        separator(),
        hbox(button("→ Signale bearbeiten", [this] { nav_level_ = 1; emit(Event::ArrowRight); }), text(" "),
             button("Space Pause/Resume", [this] { nav_level_ = 1; emit(Event::Character(' ')); }), text(" "),
             button("Del löschen", [this] { nav_level_ = 1; emit(Event::Delete); }), text(" "),
             button("Alle pausieren", [this] { action_handler_.set_all_paused(true); }), text(" "),
             button("Alle starten", [this] { action_handler_.set_all_paused(false); }),
             filler()),
    });
}

// ─── Playback ──────────────────────────────────────────────────────────────

Element TuiDataFrameSet::render_playback() {
    if (!playback_ctrl_)
        return text("No playback active.") | dim | center;

    const auto snap = playback_ctrl_->snapshot();
    if (snap.empty())
        return text("No interfaces.") | dim | center;

    // ── Message filter view (nav_level_ >= 2) ────────────────────────────
    if (nav_level_ >= 2 && playback_cursor_ < static_cast<int>(snap.size())) {
        const std::string& iface = snap[playback_cursor_].first;
        const auto msgs = playback_ctrl_->message_snapshot(iface);

        const bool any_on = std::any_of(msgs.begin(), msgs.end(), [](const auto& p) { return p.second; });

        auto toggle_row = [this](int idx) { return [this, idx] { playback_msg_cursor_ = idx; emit(Event::Return); }; };

        // "All" row at index 0
        const bool all_sel = (playback_msg_cursor_ == 0);
        Element all_row = hbox(text(all_sel ? "▶ " : "  "),
                               text(std::string(any_on ? "[✓]" : "[ ]") + " ") | (any_on ? color(Color::Green) : color(Color::Red)),
                               text("Alle Messages") | bold, filler());
        if (all_sel) all_row = all_row | inverted | focus;

        Elements rows;
        rows.push_back(clickable(std::move(all_row), toggle_row(0)));
        rows.push_back(separator());

        for (int i = 0; i < static_cast<int>(msgs.size()); ++i) {
            const auto& [name, enabled] = msgs[i];
            const bool sel = (playback_msg_cursor_ == i + 1);
            Element row = hbox(text(sel ? "▶ " : "  "),
                               text(std::string(enabled ? "[✓]" : "[ ]") + " ") | (enabled ? color(Color::Green) : color(Color::Red)),
                               text(name), filler());
            if (sel) row = row | inverted | focus;
            rows.push_back(clickable(std::move(row), toggle_row(i + 1)));
        }

        return vbox({
            hbox(button("◀ zurück", [this] { emit(Event::ArrowLeft); }), text(" "), text(iface) | bold),
            separator(),
            vbox(std::move(rows)) | vscroll_indicator | yframe | flex,
            separator(),
            hbox(text("Klick / Enter: aktivieren / deaktivieren") | dim, filler()),
        });
    }

    // ── Interface list (nav_level_ == 1) ─────────────────────────────────
    Elements rows;
    for (int i = 0; i < static_cast<int>(snap.size()); ++i) {
        const auto& [name, running] = snap[i];
        const bool sel = (i == playback_cursor_);

        Element status = running ? (text("  Running") | color(Color::Green))
                                 : (text("  Paused ") | color(Color::Yellow));

        Element row = hbox(text(sel ? "▶ " : "  "), text(name) | bold, std::move(status), text("  "),
                           button(running ? "Pause" : "Start",
                                  [this, i] { playback_cursor_ = i; nav_level_ = 1; emit(Event::Return); }, running),
                           text(" "),
                           button("Messages…", [this, i] { playback_cursor_ = i; nav_level_ = 1; emit(Event::ArrowRight); }),
                           filler());

        if (sel && nav_level_ == 1) row = row | inverted;
        if (sel)                    row = row | focus;
        rows.push_back(clickable(std::move(row), [this, i] { playback_cursor_ = i; nav_level_ = 1; }));
    }

    return vbox({
        vbox(std::move(rows)) | vscroll_indicator | yframe | flex,
        separator(),
        hbox(button("Enter starten / pausieren", [this] { nav_level_ = 1; emit(Event::Return); }), text(" "),
             button("→ Messages filtern", [this] { nav_level_ = 1; emit(Event::ArrowRight); }), filler()),
    });
}

// ─── Model tab ─────────────────────────────────────────────────────────────

Element TuiDataFrameSet::render_model() {
    if (model_outputs_.empty())
        return text("Noch keine Modell-Ausgaben.") | dim | center;

    Elements rows;
    rows.push_back(hbox({
        text("Variable") | bold | flex,
        text("Wert")     | bold | size(WIDTH, EQUAL, 24),
        text("        "),
    }));
    rows.push_back(separator());

    for (const auto& [name, value] : model_outputs_) {
        const bool on = graphed(MODEL_IFACE, "", name);
        rows.push_back(hbox({
            text(name) | flex,
            text(fmt_value(value)) | size(WIDTH, EQUAL, 24),
            button(on ? "-Graph" : "+Graph", [this, name] { toggle_graph_signal(MODEL_IFACE, "", name); }, on),
        }));
    }

    return vbox(std::move(rows)) | vscroll_indicator | yframe;
}

// ─── Graphs ────────────────────────────────────────────────────────────────

Element TuiDataFrameSet::render_plot(const GraphView& g) {
    const double t_end   = graph_paused_ ? graph_pause_t_ : now_s();
    const double t_start = t_end - g.window_s;

    // Visible points (copied: the canvas is drawn after the lock is released)
    struct Line { std::vector<std::pair<double, double>> pts; Color color; };
    auto lines = std::make_shared<std::vector<Line>>();
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (std::size_t i = 0; i < g.series.size(); ++i) {
        const auto& s = g.series[i];
        Line line{.color = series_color(static_cast<int>(i))};
        // one point before the window, so lines start at the left edge
        auto it = std::ranges::lower_bound(s.pts, t_start, {}, &std::pair<double, double>::first);
        if (it != s.pts.begin()) --it;
        for (; it != s.pts.end() && it->first <= t_end; ++it) {
            line.pts.push_back(*it);
            if (it->first >= t_start) { lo = std::min(lo, it->second); hi = std::max(hi, it->second); }
        }
        if (!g.auto_y) {
            if (s.y_min) lo = std::min(lo, *s.y_min);
            if (s.y_max) hi = std::max(hi, *s.y_max);
        }
        lines->push_back(std::move(line));
    }
    if (!(lo <= hi)) { lo = 0.0; hi = 1.0; }
    if (hi - lo < 1e-9) { lo -= 1.0; hi += 1.0; }
    else { const double pad = (hi - lo) * 0.05; lo -= pad; hi += pad; }

    Element plot = canvas([lines, t_start, t_end, lo, hi](Canvas& c) {
        const int w = c.width(), h = c.height();
        if (w < 4 || h < 4) return;
        auto px = [&](double t) { return static_cast<int>(std::lround((t - t_start) / (t_end - t_start) * (w - 1))); };
        auto py = [&](double v) { return static_cast<int>(std::lround((hi - v) / (hi - lo) * (h - 1))); };

        // zero line
        if (lo < 0.0 && hi > 0.0)
            for (int x = 0; x < w; x += 4) c.DrawPoint(x, py(0.0), true, Color::GrayDark);

        // per x column: min/max as a vertical line, connected to the previous column
        for (const auto& line : *lines) {
            int  prev_x = 0, prev_y = 0;
            bool have_prev = false;
            int  col = INT_MIN, col_min = 0, col_max = 0, col_last = 0;
            auto flush = [&] {
                if (col == INT_MIN) return;
                const int x = std::clamp(col, 0, w - 1);
                if (have_prev) c.DrawPointLine(prev_x, prev_y, x, col_min == col_last ? col_min : col_last, line.color);
                c.DrawPointLine(x, col_min, x, col_max, line.color);
                prev_x = x; prev_y = col_last; have_prev = true;
            };
            for (const auto& [t, v] : line.pts) {
                const int x = px(t);
                const int y = std::clamp(py(v), 0, h - 1);
                if (x != col) { flush(); col = x; col_min = col_max = y; }
                col_min  = std::min(col_min, y);
                col_max  = std::max(col_max, y);
                col_last = y;
            }
            flush();
        }
    });

    const double mid = (lo + hi) / 2.0;
    Element y_axis = vbox({text(fmt_value(hi)), filler(), text(fmt_value(mid)), filler(), text(fmt_value(lo))})
                   | size(WIDTH, EQUAL, 10);
    Element x_axis = hbox({text(std::format("-{}s", fmt_value(g.window_s))), filler(),
                           text(std::format("-{}s", fmt_value(g.window_s / 2))), filler(),
                           text(graph_paused_ ? "pausiert" : "jetzt")});
    return vbox({
        hbox({y_axis, separatorLight(), plot | flex}) | flex,
        hbox({text(std::string(11, ' ')), x_axis | flex}) | dim,
    });
}

Element TuiDataFrameSet::render_graphs() {
    GraphView& g = current_graph();

    std::vector<std::string> names;
    for (const auto& gv : graphs_) names.push_back(gv.name);
    Element bar = hbox({
        tab_bar(names, graph_tab_, nav_level_ == 1, [this](int i) { graph_tab_ = i; graph_cursor_ = 0; nav_level_ = 1; }),
        text(" "),
        button("n Neu", [this] { nav_level_ = 1; emit(Event::Character('n')); }),
        graphs_.size() > 1 ? hbox(text(" "), button("Graph ✕", [this] {
                                 std::lock_guard lock(mutex_);
                                 graphs_.erase(graphs_.begin() + graph_tab_);
                                 graph_tab_ = std::max(0, graph_tab_ - 1);
                             }))
                           : text(""),
        filler(),
    });

    Elements tools = {button(graph_paused_ ? "Space ▶ Live" : "Space ⏸ Pause",
                             [this] { nav_level_ = 1; emit(Event::Character(' ')); }, graph_paused_),
                      text("  Fenster: ") | dim};
    for (const double w : {5.0, 10.0, 30.0, 60.0, 120.0, 300.0}) {
        tools.push_back(button(std::format("{}s", w), [this, w] {
            std::lock_guard lock(mutex_);
            current_graph().window_s = w;
        }, std::abs(g.window_s - w) < 1e-9));
        tools.push_back(text(" "));
    }
    tools.push_back(text(" "));
    tools.push_back(button("y Auto-Y", [this] { nav_level_ = 1; emit(Event::Character('y')); }, g.auto_y));
    tools.push_back(text(" "));
    tools.push_back(button("c Leeren", [this] { nav_level_ = 1; emit(Event::Character('c')); }));
    tools.push_back(filler());

    Element content;
    if (g.series.empty()) {
        content = vbox({
            filler(),
            text("Keine Signale in diesem Graph.") | center,
            text("Trace: auf □ vor einem Signal klicken (oder Enter, Signal wählen, Enter).") | dim | center,
            text("Guards / Model: [+Graph].") | dim | center,
            filler(),
        }) | flex;
    } else {
        content = render_plot(g) | flex;
    }

    // Legend: colour, name, current value, remove
    Elements legend;
    graph_cursor_ = std::clamp(graph_cursor_, 0, std::max(0, static_cast<int>(g.series.size()) - 1));
    for (int i = 0; i < static_cast<int>(g.series.size()); ++i) {
        const auto& s   = g.series[static_cast<std::size_t>(i)];
        const bool  sel = i == graph_cursor_ && nav_level_ == 1;
        Element row = hbox({
            text(" ━━ ") | color(series_color(i)) | bold,
            text(series_label(s.iface, s.msg, s.signal)),
            text(s.iface.empty() || s.msg.empty() ? "" : "  (" + s.iface + ")") | dim,
            filler(),
            text(s.pts.empty() ? "-" : fmt_value(s.pts.back().second)) | bold,
            text("  "),
            button("✕", [this, i] { graph_cursor_ = i; nav_level_ = 1; emit(Event::Delete); }),
        });
        if (sel) row = row | inverted | focus;
        legend.push_back(clickable(row, [this, i] { graph_cursor_ = i; nav_level_ = 1; }));
    }

    Elements body = {bar, hbox(std::move(tools)), separator(), content};
    if (!legend.empty()) {
        body.push_back(separator());
        body.push_back(vbox(std::move(legend)) | vscroll_indicator | yframe | size(HEIGHT, LESS_THAN, 8));
    }
    return vbox(std::move(body));
}

// ─── Status line (log prompt / messages / buttons) ─────────────────────────

Element TuiDataFrameSet::render_status_line() {
    if (log_prompt_) {
        const char* what = config_ && config_->logging.format == "both" ? " (.mcap + .blf)" : "";
        return hbox({text("Log-Datei: ") | bold,
                     // keep the end (cursor) visible on narrow terminals
                     text(log_path_buf_.size() > 70 ? "…" + log_path_buf_.substr(log_path_buf_.size() - 69)
                                                    : log_path_buf_),
                     text("_") | inverted,
                     filler(),
                     text(std::string(what) + " ") | dim,
                     button("Enter Start", [this] { emit(Event::Return); }, false, true),
                     text(" "),
                     button("Esc Abbrechen", [this] { emit(Event::Escape); }, false, true),
                     text(" ")});
    }
    const bool recent = !message_.empty() &&
                        std::chrono::steady_clock::now() - message_time_ < std::chrono::seconds(8);

    Element msg = recent ? text(message_) | (message_error_ ? color(Color::Red) : dim) | flex_shrink
                         : text("");
    Elements keys;
    if (log_) {
        const bool rec = log_->is_logging();
        keys.push_back(button(rec ? "l Log stoppen" : "l Log starten", [this] { toggle_log(); }, rec));
        keys.push_back(text(" "));
        keys.push_back(button("o Viewer", [this] { open_viewer(); }));
        keys.push_back(text(" "));
    }
    keys.push_back(button("q Beenden", [this] { screen_.ExitLoopClosure()(); }));
    keys.push_back(text(" "));
    return hbox({msg, filler(), hbox(std::move(keys))});
}

// ─── Top-level render ──────────────────────────────────────────────────────

Element TuiDataFrameSet::render() {
    std::lock_guard lock(mutex_);
    click_targets_.clear();

    const auto               visible = tabs();
    std::vector<std::string> tab_labels;
    int                      selected = 0;
    for (std::size_t i = 0; i < visible.size(); ++i) {
        switch (visible[i]) {
            case Tab::Trace:    tab_labels.push_back("Trace [t]");    break;
            case Tab::Send:     tab_labels.push_back("Send [s]");     break;
            case Tab::Actions:  tab_labels.push_back("Actions [a]");  break;
            case Tab::Guards:   tab_labels.push_back("Guards [g]");   break;
            case Tab::Graphs:   tab_labels.push_back("Graphs [v]");   break;
            case Tab::Model:    tab_labels.push_back("Model [m]");    break;
            case Tab::Playback: tab_labels.push_back("Playback [p]"); break;
        }
        if (visible[i] == main_tab_) selected = static_cast<int>(i);
    }

    const Element main_tabs = tab_bar(tab_labels, selected, nav_level_ == 0,
                                      [this, visible](int i) { select_tab(visible[static_cast<std::size_t>(i)]); });

    Element content;
    switch (main_tab_) {
        case Tab::Trace:    content = render_trace();    break;
        case Tab::Send:     content = render_send();     break;
        case Tab::Actions:  content = render_actions();  break;
        case Tab::Guards:   content = render_guards();   break;
        case Tab::Graphs:   content = render_graphs();   break;
        case Tab::Model:    content = render_model();    break;
        case Tab::Playback: content = render_playback(); break;
    }

    Element title = text(" caneo ");
    if (log_ && log_->is_logging()) {
        const int secs = static_cast<int>(log_->elapsed_seconds());
        title = hbox({title,
                      text(std::format(" ● REC {:02}:{:02} ", secs / 60, secs % 60)) | color(Color::Red) | bold,
                      text(std::filesystem::path(log_->current_path()).filename().string() + " ") | dim});
    }
    if (status_connected_fn_) {
        const bool connected = status_connected_fn_();
        Element dot   = text("● ") | color(connected ? Color::Green : Color::Red);
        Element label = text(status_label_);
        Element indicator = connected
            ? hbox({dot, label, text(" ")})
            : hbox({dot, label, text(" (reconnecting...) ") | dim});
        title = hbox({title, filler(), indicator});
    }

    return window(title, vbox({main_tabs, separator(), content | flex, separator(), render_status_line()}));
}
