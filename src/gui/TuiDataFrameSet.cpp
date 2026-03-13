#include "TuiDataFrameSet.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>

using namespace ftxui;

// ─── Construction ──────────────────────────────────────────────────────────

TuiDataFrameSet::TuiDataFrameSet(const std::vector<InterfaceConfig>& iface_configs,
                                 ActionHandler&                      action_handler)
    : action_handler_(action_handler)
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

// ─── Event loop ────────────────────────────────────────────────────────────

void TuiDataFrameSet::run() {
    action_handler_.set_notify([this] { screen_.PostEvent(Event::Custom); });

    auto renderer = Renderer([this] { return render(); });

    renderer |= CatchEvent([this](Event event) -> bool {

        // ── Period input ──────────────────────────────────────────────────
        if (send_period_editing_) {
            if (event == Event::Return || event == Event::ArrowLeft || event == Event::ArrowRight) {
                if (!send_period_buf_.empty()) {
                    try {
                        const auto ms = static_cast<long>(std::stod(send_period_buf_));
                        if (ms > 0)
                            create_periodic_action(std::chrono::milliseconds(ms));
                    } catch (...) {}
                }
                send_period_editing_ = false;
                send_period_buf_.clear();
                return true;
            }
            if (event == Event::Escape) {
                send_period_editing_ = false;
                send_period_buf_.clear();
                return true;
            }
            if (event == Event::Backspace) {
                if (!send_period_buf_.empty()) send_period_buf_.pop_back();
                return true;
            }
            if (event.is_character()) {
                const char c = event.character()[0];
                if (std::isdigit(static_cast<unsigned char>(c)))
                    send_period_buf_ += c;
                return true;
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

        // ── Quit ──────────────────────────────────────────────────────────
        if (event == Event::Character('q') || event == Event::Escape) {
            screen_.ExitLoopClosure()();
            return true;
        }

        // ── Tab shortcuts ─────────────────────────────────────────────────
        if (event == Event::Character('t')) { main_tab_ = 0; nav_level_ = 1; return true; }
        if (event == Event::Character('s')) { main_tab_ = 1; nav_level_ = 1; return true; }
        if (event == Event::Character('a')) { main_tab_ = 2; nav_level_ = 1; return true; }

        // ── Signal list (nav_level_ == 3) ─────────────────────────────────
        if (nav_level_ == 3) {
            if (event == Event::ArrowLeft) {
                nav_level_ = 2;
                return true;
            }
            if (event == Event::ArrowRight) {
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
                            send_period_editing_ = true;
                            send_period_buf_.clear();
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
            return false;
        }

        // ── Message list (nav_level_ == 2) ────────────────────────────────
        if (nav_level_ == 2) {
            if (event == Event::ArrowLeft) {
                nav_level_ = 1;
                return true;
            }
            if (event == Event::ArrowRight) {
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
                nav_level_ = 0;
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
                    if (!sets_.empty())
                        sub_tab_trace_ = (sub_tab_trace_ + 1) % static_cast<int>(sets_.size());
                } else {
                    if (!interfaces_.empty())
                        sub_tab_send_ = (sub_tab_send_ + 1) % static_cast<int>(interfaces_.size());
                }
                return true;
            }
            if (event == Event::ArrowLeft) {
                if (main_tab_ == 0) {
                    std::lock_guard lock(mutex_);
                    if (!sets_.empty())
                        sub_tab_trace_ = (sub_tab_trace_ + static_cast<int>(sets_.size()) - 1)
                                         % static_cast<int>(sets_.size());
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
        if (event == Event::ArrowRight) {
            main_tab_ = (main_tab_ + 1) % 3;
            return true;
        }
        if (event == Event::ArrowLeft) {
            main_tab_ = (main_tab_ + 2) % 3;
            return true;
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

    Elements frame_rows;
    for (const auto& [id, frame] : active_set.frames()) {
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

        Elements frame_block = {
            hbox(text(id_str) | bold,
                 text(" | DLC:" + std::to_string(frame.header().dlc)),
                 text(delta_str) | dim,
                 has_decoded ? text("") : text(" | " + hex_str) | dim),
        };
        for (const auto& sig : frame.decoded())
            frame_block.push_back(
                text("    " + sig.name + ": " + std::to_string(sig.value) + " " + sig.unit));
        frame_rows.push_back(vbox(std::move(frame_block)));
    }
    if (frame_rows.empty())
        frame_rows.push_back(text("(no frames)") | dim);

    return vbox({sub_tabs, separator(), vbox(std::move(frame_rows))});
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

    Element periodic_row;
    if (cur_periodic && send_period_editing_) {
        periodic_row = hbox(
            text("▶ "),
            text("[ Periodic Action ]   Periode: ["),
            text(send_period_buf_),
            text("_] ms")) | bold | inverted;
    } else {
        periodic_row = hbox(text(cur_periodic ? "▶ " : "  "), text("[ Periodic Action ]"));
        if (cur_periodic && nav_level_ == 3) periodic_row = periodic_row | bold | inverted;
    }
    if (cur_periodic) periodic_row = periodic_row | focus;
    sig_rows.push_back(std::move(periodic_row));

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

// ─── Actions ───────────────────────────────────────────────────────────────

Element TuiDataFrameSet::render_actions() const {
    const auto snap = action_handler_.snapshot();

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
            "{}0x{:03X}  {:<20}  {:<8}  {}  {}  {}",
            sel ? "▶ " : "  ",
            a.msg_id,
            a.msg_name,
            a.is_periodic ? "Periodic" : "Single",
            period_str,
            a.interface,
            last_str);

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
        text("[Del] Action löschen") | dim,
    });
}

// ─── Top-level render ──────────────────────────────────────────────────────

Element TuiDataFrameSet::render() const {
    std::lock_guard lock(mutex_);

    const Element main_tabs =
        make_tab_bar({"Trace", "Send", "Actions"}, main_tab_, nav_level_ == 0);

    Element content;
    switch (main_tab_) {
        case 0:  content = render_trace();   break;
        case 1:  content = render_send();    break;
        default: content = render_actions(); break;
    }

    return window(text(" caneo "), vbox({main_tabs, separator(), content}));
}
