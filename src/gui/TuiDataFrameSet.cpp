#include "TuiDataFrameSet.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>

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

void TuiDataFrameSet::run() {
    action_handler_.set_notify([this] { screen_.PostEvent(Event::Custom); });

    auto renderer = Renderer([this] { return render(); });

    renderer |= CatchEvent([this](Event event) -> bool {

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
            return false;
        }

        // ── Quit ──────────────────────────────────────────────────────────
        if (event == Event::Character('q') || event == Event::Escape) {
            screen_.ExitLoopClosure()();
            return true;
        }

        // ── Tab shortcuts ─────────────────────────────────────────────────
        if (event == Event::Character('t')) { main_tab_ = 0; nav_level_ = 1; return true; }
        if (event == Event::Character('s')) { main_tab_ = 1; nav_level_ = 1; return true; }
        if (event == Event::Character('a')) { main_tab_ = 2; nav_level_ = 1; return true; }
        if (event == Event::Character('p') && playback_ctrl_) {
            main_tab_ = 3; nav_level_ = 1; return true;
        }
        if (event == Event::Character('f') && main_tab_ == 0) {
            trace_searching_ = true;
            return true;
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
        if (nav_level_ == 2 && main_tab_ == 3 && playback_ctrl_) {
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

        // ── nav_level_ == 2: message list (Send) or action signal view (Actions) ──
        if (nav_level_ == 2) {
            if (main_tab_ == 2) {
                // Actions tab: signal edit view
                if (event == Event::ArrowLeft) {
                    nav_level_ = 1;
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
            if (main_tab_ == 3 && playback_ctrl_) {
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

            // Actions tab: navigate action list
            if (main_tab_ == 2) {
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
                if (main_tab_ == 0 && trace_cursor_ > 0) {
                    --trace_cursor_;
                } else {
                    nav_level_ = 0;
                }
                return true;
            }
            if (event == Event::ArrowDown && main_tab_ == 0) {
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
            if (event == Event::ArrowDown && main_tab_ == 1) {
                const SendModel* model = selected_send_model();
                if (model && !model->messages().empty()) {
                    send_msg_cursor_ = 0;
                    nav_level_       = 2;
                }
                return true;
            }
            if (event == Event::ArrowRight) {
                if (main_tab_ == 0) {
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
                if (main_tab_ == 0) {
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
        {
            const int tab_count = playback_ctrl_ ? 4 : 3;
            if (event == Event::ArrowRight) {
                main_tab_ = (main_tab_ + 1) % tab_count;
                return true;
            }
            if (event == Event::ArrowLeft) {
                main_tab_ = (main_tab_ + tab_count - 1) % tab_count;
                return true;
            }
        }
        return false;
    });

    screen_.Loop(renderer);
}

// ─── Rendering helpers ─────────────────────────────────────────────────────

static Element make_tab_bar(const std::vector<std::string>& labels, int selected, bool focused) {
    Elements tabs;
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        Element label = text(" " + labels[i] + " ");
        if (i == selected)
            label = label | inverted | (focused ? bold : dim);
        tabs.push_back(std::move(label));
        if (i + 1 < static_cast<int>(labels.size()))
            tabs.push_back(text("│"));
    }
    return hbox(std::move(tabs));
}

// ─── Trace ─────────────────────────────────────────────────────────────────

Element TuiDataFrameSet::render_trace() const {
    std::vector<std::string> iface_names;
    for (const auto& [name, _] : sets_)
        iface_names.push_back(name);

    Element sub_tabs = make_tab_bar(iface_names, sub_tab_trace_, nav_level_ == 1);

    if (iface_names.empty())
        return vbox({sub_tabs, separator(), text("Waiting for frames...") | dim | center});

    const int           count      = static_cast<int>(iface_names.size());
    const int           idx        = sub_tab_trace_ < count ? sub_tab_trace_ : count - 1;
    const DataFrameSet& active_set = sets_.at(iface_names[idx]);

    // Helper: render one frame entry
    auto make_frame_elem = [&](uint32_t id, const CanFrame& frame) -> Element {
        const std::string id_str = std::format("0x{:03X}", id);

        std::string delta_str = " | Δ ---    ";
        if (const auto d = active_set.delta(id)) {
            const double ms = std::chrono::duration<double, std::milli>(*d).count();
            delta_str = std::format(" | Δ {:6.1f}ms", ms);
        }

        std::string hex_str;
        for (const uint8_t byte : frame.payload())
            hex_str += std::format("{:02X} ", byte);
        if (!hex_str.empty()) hex_str.pop_back();

        const bool has_decoded = !frame.decoded().empty();

        const std::string& name = frame.msg_name();
        Elements frame_block = {
            hbox(text(id_str) | bold,
                 name.empty() ? text("") : text("  " + name) | bold,
                 text(" | DLC:" + std::to_string(frame.header().dlc)),
                 text(delta_str) | dim,
                 has_decoded ? text("") : text(" | " + hex_str) | dim),
        };
        for (const auto& sig : frame.decoded())
            frame_block.push_back(
                text("    " + sig.name + ": " + std::to_string(sig.value) + " " + sig.unit));
        return vbox(std::move(frame_block));
    };

    // Build frame rows with cursor highlight
    const auto& all_frames = active_set.frames();
    int row_idx = 0;

    auto make_scrollable_rows = [&](const std::vector<std::pair<uint32_t, const CanFrame*>>& entries) {
        Elements rows;
        for (const auto& [id, frame_ptr] : entries) {
            const bool selected = (row_idx == trace_cursor_) && (nav_level_ >= 1);
            Element elem = make_frame_elem(id, *frame_ptr);
            if (selected) elem = elem | inverted | focus;
            rows.push_back(std::move(elem));
            ++row_idx;
        }
        return rows;
    };

    // Collect frame entries (ordered by id, as stored in the map)
    std::vector<std::pair<uint32_t, const CanFrame*>> all_entries;
    for (const auto& [id, frame] : all_frames)
        all_entries.emplace_back(id, &frame);

    // Without active search
    if (!trace_searching_ || trace_search_buf_.empty()) {
        Elements frame_rows = make_scrollable_rows(all_entries);
        if (frame_rows.empty())
            frame_rows.push_back(text("(no frames)") | dim);

        Element content = vbox(std::move(frame_rows)) | vscroll_indicator | frame;

        if (trace_searching_) {
            Element search_bar = hbox(text("Suche: [") | dim,
                                      text(trace_search_buf_),
                                      text("_") | inverted,
                                      text("]") | dim);
            return vbox({sub_tabs, separator(), search_bar, separator(), content});
        }
        return vbox({sub_tabs, separator(), content});
    }

    // Active search with non-empty query: split into matched / unmatched
    std::vector<std::pair<uint32_t, const CanFrame*>> matched_entries, unmatched_entries;
    for (const auto& [id, frame_ptr] : all_entries) {
        if (trace_matches(id, *frame_ptr, trace_search_buf_))
            matched_entries.emplace_back(id, frame_ptr);
        else
            unmatched_entries.emplace_back(id, frame_ptr);
    }
    
    Elements matched_rows = make_scrollable_rows(matched_entries);
    Elements unmatched_rows = make_scrollable_rows(unmatched_entries);
    if (matched_rows.empty())
        matched_rows.push_back(text("(keine Treffer)") | dim);
    if (unmatched_rows.empty())
        unmatched_rows.push_back(text("(keine weiteren Frames)") | dim);

    Element search_bar = hbox(text("Suche: [") | dim,
                               text(trace_search_buf_),
                               text("_") | inverted,
                               text("]") | dim);

    return vbox({
        sub_tabs,
        separator(),
        search_bar,
        separator(),
        vbox(std::move(matched_rows)) | vscroll_indicator | frame | flex,
        separator(),
        vbox(std::move(unmatched_rows)) | vscroll_indicator | frame,
    });
}

// ─── Send: message list ────────────────────────────────────────────────────

Element TuiDataFrameSet::render_msg_list(const SendModel& model) const {
    const auto& msgs = model.messages();
    Elements    rows;
    for (int i = 0; i < static_cast<int>(msgs.size()); ++i) {
        const bool sel   = (i == send_msg_cursor_);
        Element    row   = text(std::format("{}0x{:03X}  {}",
                                             sel ? "▶ " : "  ", msgs[i].id, msgs[i].name));
        if (sel && nav_level_ == 2) row = row | bold | inverted;
        else if (sel)               row = row | bold;
        if (sel)                    row = row | focus;
        rows.push_back(std::move(row));
    }
    if (rows.empty())
        rows.push_back(text("(no messages in DBC)") | dim);
    return vbox(std::move(rows)) | vscroll_indicator | frame;
}

// ─── Send: signal list ─────────────────────────────────────────────────────

Element TuiDataFrameSet::render_sig_list(const SendModel& model) const {
    const auto& msgs    = model.messages();
    const int   msg_idx = std::min(send_msg_cursor_, static_cast<int>(msgs.size()) - 1);
    if (msg_idx < 0) return text("(no message selected)") | dim;

    const SendMessage& msg = msgs[msg_idx];
    Element header = text(std::format("◀ 0x{:03X}  {}", msg.id, msg.name)) | bold;

    Elements sig_rows;
    for (int i = 0; i < static_cast<int>(msg.signals.size()); ++i) {
        const SendSignal& sig     = msg.signals[i];
        const bool        is_cur  = (i == send_sig_cursor_);
        const bool        editing = is_cur && send_editing_;

        Element val_elem;
        if (editing)
            val_elem = hbox(text("["), text(send_edit_buf_), text("_]")) | inverted;
        else
            val_elem = text(std::format("{:.6g}", sig.value));

        Element row = hbox(
            text(is_cur ? "▶ " : "  "),
            text(sig.name + ": ") | (is_cur ? bold : nothing),
            val_elem,
            text(" " + sig.unit) | dim);

        if (is_cur && !editing && nav_level_ == 3) row = row | inverted;
        if (is_cur)                                  row = row | focus;
        sig_rows.push_back(std::move(row));
    }
    if (sig_rows.empty())
        sig_rows.push_back(text("(no signals)") | dim);

    // ── Action buttons below the signal list ──────────────────────────────
    const int  sig_count    = static_cast<int>(msg.signals.size());
    const bool cur_single   = (send_sig_cursor_ == sig_count);
    const bool cur_periodic = (send_sig_cursor_ == sig_count + 1);

    sig_rows.push_back(text("")); // spacing

    Element single_row = hbox(text(cur_single ? "▶ " : "  "), text("[ Single Action ]"));
    if (cur_single && nav_level_ == 3) single_row = single_row | bold | inverted;
    if (cur_single)                    single_row = single_row | focus;
    sig_rows.push_back(std::move(single_row));

    using PS = PeriodicStep;
    const bool in_periodic = (periodic_step_ != PS::None);

    Element periodic_row;
    if (cur_periodic && periodic_step_ == PS::Period) {
        periodic_row = hbox(
            text("▶ [ Periodic Action ]   Interval: ["),
            text(periodic_interval_buf_),
            text("_] ms")) | bold | inverted;
    } else if (cur_periodic && in_periodic) {
        periodic_row = hbox(
            text("▶ [ Periodic Action ]   Interval: "),
            text(std::to_string(periodic_interval_ms_) + " ms")) | bold | inverted;
    } else {
        periodic_row = hbox(text(cur_periodic ? "▶ " : "  "), text("[ Periodic Action ]"));
        if (cur_periodic && nav_level_ == 3) periodic_row = periodic_row | bold | inverted;
    }
    if (cur_periodic) periodic_row = periodic_row | focus;
    sig_rows.push_back(std::move(periodic_row));

    // TypeSelect
    if (cur_periodic && periodic_step_ == PS::TypeSelect) {
        const bool on_const = (periodic_type_cursor_ == 0);
        Element const_elem = text("[Constant]");
        if (on_const)  const_elem = const_elem | bold | inverted;
        else           const_elem = const_elem | dim;
        Element sin_elem = text("[Sin]");
        if (!on_const) sin_elem = sin_elem | bold | inverted;
        else           sin_elem = sin_elem | dim;
        sig_rows.push_back(hbox(
            text("    "),
            text(on_const  ? "▶ " : "  "), std::move(const_elem),
            text("   "),
            text(!on_const ? "▶ " : "  "), std::move(sin_elem)));
    }

    // Sin parameter fields (shown once we're past TypeSelect)
    if (cur_periodic && static_cast<int>(periodic_step_) >= static_cast<int>(PS::SinAmplitude)) {
        auto field = [&](const std::string& label, const std::string& buf,
                         const std::string& unit, PS step) {
            const bool active = (periodic_step_ == step);
            Element val = active
                ? hbox(text("["), text(buf), text("_]"))
                : text(buf.empty() ? "-" : buf);
            Element row = hbox(text("    "), text(label) | dim, val, text(" " + unit) | dim);
            sig_rows.push_back(active ? (row | bold | inverted) : row);
        };
        field("Amplitude: ", sin_amplitude_buf_, "",   PS::SinAmplitude);
        field("Period:    ", sin_period_buf_,    "ms", PS::SinPeriod);
        field("Offset:    ", sin_offset_buf_,    "",   PS::SinOffset);

        Element start = hbox(text("    "), text("[ Start ]"));
        sig_rows.push_back(periodic_step_ == PS::SinReady ? (start | bold | inverted) : (start | dim));
    }

    // ── Raw frame preview ─────────────────────────────────────────────────
    const std::vector<uint8_t> raw = model.encode(static_cast<std::size_t>(msg_idx));
    std::string hex_str;
    for (const uint8_t b : raw) hex_str += std::format("{:02X} ", b);
    if (!hex_str.empty()) hex_str.pop_back();

    return vbox({
        header,
        separator(),
        vbox(std::move(sig_rows)) | vscroll_indicator | frame,
        separator(),
        hbox(text("Raw: ") | dim, text(hex_str) | bold),
        separator(),
        hbox(text("[1]") | bold, text(" Single Action   "),
             text("[2]") | bold, text(" Periodic Action")) | dim,
    });
}

// ─── Send ──────────────────────────────────────────────────────────────────

Element TuiDataFrameSet::render_send() const {
    const int count    = static_cast<int>(interfaces_.size());
    const int idx      = count > 0 ? std::min(sub_tab_send_, count - 1) : 0;
    Element   sub_tabs = make_tab_bar(interfaces_, idx, nav_level_ == 1);

    Element content;
    const SendModel* model = selected_send_model();
    if (!model)
        content = text("No DBC configured for this interface.") | dim | center;
    else if (nav_level_ >= 3)
        content = render_sig_list(*model);
    else
        content = render_msg_list(*model);

    return vbox({sub_tabs, separator(), content});
}

// ─── Action signal edit view ───────────────────────────────────────────────

Element TuiDataFrameSet::render_action_signals(const ActionInfo& info) const {
    auto it = send_models_.find(info.interface);
    if (it == send_models_.end())
        return text("Kein SendModel für dieses Interface.") | dim | center;

    const SendModel& model = *it->second;
    const auto& msgs = model.messages();

    // find message by id
    const SendMessage* msg_ptr = nullptr;
    int msg_idx = -1;
    for (int i = 0; i < static_cast<int>(msgs.size()); ++i) {
        if (msgs[i].id == info.msg_id) { msg_ptr = &msgs[i]; msg_idx = i; break; }
    }
    if (!msg_ptr || msg_idx < 0)
        return text("Message nicht in DBC gefunden.") | dim | center;

    const SendMessage& msg = *msg_ptr;
    Element header = text(std::format("◀ [{}] 0x{:03X}  {}  ({}, {}ms interval)",
                                      info.interface, msg.id, msg.name,
                                      info.type_name,
                                      info.period.count())) | bold;

    // Sin action: show sin parameters instead of signal list
    if (info.type_name == "Sin") {
        const std::array<std::pair<std::string, std::string>, 3> fields{{
            {"Amplitude: ", std::format("{:.6g}", info.sin_amplitude)},
            {"Period:    ", std::format("{} ms",  info.sin_period_ms)},
            {"Offset:    ", std::format("{:.6g}", info.sin_offset)},
        }};
        Elements rows;
        for (int i = 0; i < 3; ++i) {
            const bool is_cur = (i == actions_sig_cursor_);
            const bool editing = is_cur && actions_editing_;
            Element val = editing
                ? (hbox(text("["), text(actions_edit_buf_), text("_]")) | inverted)
                : text(fields[i].second);
            Element row = hbox(
                text(is_cur ? "▶ " : "  "),
                text(fields[i].first) | (is_cur ? bold : nothing),
                val);
            if (is_cur && !editing && nav_level_ == 2) row = row | inverted;
            if (is_cur) row = row | focus;
            rows.push_back(std::move(row));
        }
        return vbox({header, separator(), vbox(std::move(rows)) | frame});
    }

    Elements sig_rows;
    for (int i = 0; i < static_cast<int>(msg.signals.size()); ++i) {
        const SendSignal& sig    = msg.signals[i];
        const bool        is_cur = (i == actions_sig_cursor_);
        const bool        editing = is_cur && actions_editing_;

        Element val_elem;
        if (editing)
            val_elem = hbox(text("["), text(actions_edit_buf_), text("_]")) | inverted;
        else
            val_elem = text(std::format("{:.6g}", sig.value));

        Element row = hbox(
            text(is_cur ? "▶ " : "  "),
            text(sig.name + ": ") | (is_cur ? bold : nothing),
            val_elem,
            text(" " + sig.unit) | dim);

        if (is_cur && !editing && nav_level_ == 2) row = row | inverted;
        if (is_cur)                                  row = row | focus;
        sig_rows.push_back(std::move(row));
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
        vbox(std::move(sig_rows)) | vscroll_indicator | frame,
        separator(),
        hbox(text("Raw: ") | dim, text(hex_str) | bold),
    });
}

// ─── Actions ───────────────────────────────────────────────────────────────

Element TuiDataFrameSet::render_actions() const {
    const auto snap = action_handler_.snapshot();

    // Signal edit view for a specific action
    if (nav_level_ == 2 && actions_cursor_ < static_cast<int>(snap.size()))
        return render_action_signals(snap[actions_cursor_]);

    if (snap.empty()) {
        return vbox({
            text("Keine Actions definiert.") | dim | center,
            separator(),
            text("[s] SingleAction  [p] PeriodicAction  im Send-Tab erstellen") | dim | center,
        });
    }

    Elements rows;
    for (int i = 0; i < static_cast<int>(snap.size()); ++i) {
        const auto& a   = snap[i];
        const bool  sel = (i == actions_cursor_);

        std::string period_str = a.is_periodic
            ? std::format("{:>6}ms", a.period.count())
            : "      -  ";

        std::string last_str = "never";
        if (a.ever_sent) {
            const auto ago = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - a.last_sent);
            if (ago.count() < 1000)
                last_str = std::format("{:4}ms ago", ago.count());
            else
                last_str = std::format("{:.1f}s ago",
                                       static_cast<double>(ago.count()) / 1000.0);
        }

        const std::string label = std::format(
            "{}0x{:03X}  {:<20}  {:<8}  {}  {}  {}  {}",
            sel ? "▶ " : "  ",
            a.msg_id,
            a.msg_name,
            a.type_name,
            period_str,
            a.interface,
            last_str,
            a.paused ? "[paused]" : "");

        Element row = text(label);
        if (sel && nav_level_ == 1) row = row | bold | inverted;
        else if (sel)               row = row | bold;
        if (sel)                    row = row | focus;
        rows.push_back(std::move(row));
    }

    return vbox({
        text(std::format("  {:<5}  {:<20}  {:<8}  {:<9}  {:<6}  {}",
                         "ID", "Name", "Typ", "Periode", "Iface", "Letzter Send")) | dim,
        separator(),
        vbox(std::move(rows)) | vscroll_indicator | frame,
        separator(),
        hbox(text("[→]") | bold, text(" Signale bearbeiten  "),
             text("[Space]") | bold, text(" Pause/Resume  "),
             text("[Del]") | bold, text(" Action löschen")) | dim,
    });
}

// ─── Playback ──────────────────────────────────────────────────────────────

Element TuiDataFrameSet::render_playback() const {
    if (!playback_ctrl_)
        return text("No playback active.") | dim | center;

    const auto snap = playback_ctrl_->snapshot();
    if (snap.empty())
        return text("No interfaces.") | dim | center;

    // ── Message filter view (nav_level_ >= 2) ────────────────────────────
    if (nav_level_ >= 2 && playback_cursor_ < static_cast<int>(snap.size())) {
        const std::string& iface = snap[playback_cursor_].first;
        const auto msgs = playback_ctrl_->message_snapshot(iface);

        const bool any_on = std::any_of(msgs.begin(), msgs.end(),
                                        [](const auto& p) { return p.second; });

        // "All" row at index 0
        const bool all_sel = (playback_msg_cursor_ == 0);
        const std::string all_check = any_on ? "[✓]" : "[ ]";
        Element all_row = hbox(
            text(all_sel ? "▶ " : "  "),
            text(all_check + " ") | (any_on ? color(Color::Green) : color(Color::Red)),
            text("Alle Messages") | bold);
        if (all_sel) all_row = all_row | inverted | focus;

        Elements rows;
        rows.push_back(std::move(all_row));
        rows.push_back(separator());

        for (int i = 0; i < static_cast<int>(msgs.size()); ++i) {
            const auto& [name, enabled] = msgs[i];
            const bool sel = (playback_msg_cursor_ == i + 1);
            const std::string check = enabled ? "[✓]" : "[ ]";
            Element row = hbox(
                text(sel ? "▶ " : "  "),
                text(check + " ") | (enabled ? color(Color::Green) : color(Color::Red)),
                text(name));
            if (sel) row = row | inverted | focus;
            rows.push_back(std::move(row));
        }

        return vbox({
            hbox(text("◀ ") | dim, text(iface) | bold),
            separator(),
            vbox(std::move(rows)) | vscroll_indicator | frame,
            separator(),
            hbox(text("[Enter]") | bold, text(" aktivieren / deaktivieren  "),
                 text("[←]") | bold, text(" zurück")) | dim,
        });
    }

    // ── Interface list (nav_level_ == 1) ─────────────────────────────────
    Elements rows;
    for (int i = 0; i < static_cast<int>(snap.size()); ++i) {
        const auto& [name, running] = snap[i];
        const bool sel = (i == playback_cursor_);

        Element status = running
            ? (text("  Running") | color(Color::Green))
            : (text("  Paused ") | color(Color::Yellow));

        Element row = hbox(
            text(sel ? "▶ " : "  "),
            text(name) | bold,
            std::move(status));

        if (sel && nav_level_ == 1) row = row | inverted;
        if (sel)                    row = row | focus;
        rows.push_back(std::move(row));
    }

    return vbox({
        vbox(std::move(rows)) | vscroll_indicator | frame,
        separator(),
        hbox(text("[Enter]") | bold, text(" starten / pausieren  "),
             text("[→]") | bold, text(" Messages filtern")) | dim,
    });
}

// ─── Top-level render ──────────────────────────────────────────────────────

Element TuiDataFrameSet::render() const {
    std::lock_guard lock(mutex_);

    std::vector<std::string> tab_labels = {"Trace", "Send", "Actions"};
    if (playback_ctrl_) tab_labels.push_back("Playback");

    const Element main_tabs = make_tab_bar(tab_labels, main_tab_, nav_level_ == 0);

    Element content;
    switch (main_tab_) {
        case 0:  content = render_trace();    break;
        case 1:  content = render_send();     break;
        case 2:  content = render_actions();  break;
        default: content = render_playback(); break;
    }

    return window(text(" caneo "), vbox({main_tabs, separator(), content}));
}
