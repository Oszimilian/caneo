#pragma once

#include "GuiDataFrameSet.hpp"
#include "action/ActionHandler.hpp"
#include "config/Config.hpp"
#include "frame/DataFrameSet.hpp"
#include "send/SendModel.hpp"

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class TuiDataFrameSet : public GuiDataFrameSet {
public:
    TuiDataFrameSet(const std::vector<InterfaceConfig>& iface_configs,
                    ActionHandler&                      action_handler);

    void update(const CanFrame& frame) override;
    void run() override;

private:
    // Rendering
    ftxui::Element render() const;
    ftxui::Element render_trace() const;
    ftxui::Element render_send() const;
    ftxui::Element render_msg_list(const SendModel& model) const;
    ftxui::Element render_sig_list(const SendModel& model) const;
    ftxui::Element render_actions() const;

    // Helpers
    const std::string& selected_send_iface() const;
    SendModel*         selected_send_model();
    const SendModel*   selected_send_model() const;

    void create_single_action();
    void create_periodic_action(std::chrono::milliseconds period);

    // Interface + send models
    std::vector<std::string>                          interfaces_;
    std::map<std::string, std::unique_ptr<SendModel>> send_models_;

    // Action handler (externally owned)
    ActionHandler& action_handler_;

    // ── Navigation (ftxui thread only) ───────────────────────────────────
    // nav_level_: 0=MainTabs  1=SubTabs/ActionList  2=MsgList  3=SigList
    int  nav_level_       = 0;
    int  main_tab_        = 0; // 0=Trace  1=Send  2=Actions
    int  sub_tab_trace_   = 0;
    int  sub_tab_send_    = 0;
    int  send_msg_cursor_ = 0;
    int  send_sig_cursor_ = 0;
    int  actions_cursor_  = 0;

    // Signal value editing
    bool        send_editing_      = false;
    std::string send_edit_buf_;

    // Period input (for PeriodicAction creation)
    bool        send_period_editing_ = false;
    std::string send_period_buf_;

    // ── Trace data (guarded by mutex_) ────────────────────────────────────
    mutable std::mutex                  mutex_;
    std::map<std::string, DataFrameSet> sets_;

    ftxui::ScreenInteractive screen_ = ftxui::ScreenInteractive::Fullscreen();
};
