#pragma once

#include "SendWindow.hpp"
#include "Window.hpp"
#include "action/ActionHandler.hpp"

#include <cstddef>
#include <vector>

class ActionsWindow : public Window {
public:
    ActionsWindow(ActionHandler& action_handler, SendWindow& send_window);

    void render() override;

private:
    void open_edit(const ActionInfo& info);
    void render_edit_window(const std::vector<ActionInfo>& actions);

    SendModel* find_model(const std::string& interface);
    int        find_msg_idx(const SendModel& model, uint64_t msg_id);

    ActionHandler& action_handler_;
    SendWindow&    send_window_;

    // edit window state
    bool                edit_open_       = false;
    std::size_t         edit_action_idx_ = 0;
    std::vector<double> edit_values_;     // for Constant actions

    // sin edit state (initialized from ActionInfo on open)
    double              edit_amplitude_    = 0.0;
    double              edit_offset_       = 0.0;
    int                 edit_sin_period_   = 1000;
};
