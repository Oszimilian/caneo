#pragma once

#include "Window.hpp"
#include "action/ActionHandler.hpp"
#include "config/Config.hpp"
#include "send/SendModel.hpp"

#include <string>
#include <vector>

class SendWindow : public Window {
public:
    struct IfaceData {
        std::string name;
        SendModel   model;
    };

    SendWindow(const std::vector<InterfaceConfig>& iface_configs,
               ActionHandler& action_handler);

    void render() override;

    std::vector<IfaceData>& ifaces() { return ifaces_; }

private:

    void open_popup(int iface_idx, int msg_idx);
    void render_edit_window();

    ActionHandler&         action_handler_;
    std::vector<IfaceData> ifaces_;

    // edit window state
    bool                popup_open_  = false;
    int                 popup_iface_ = -1;
    int                 popup_msg_   = -1;
    std::vector<double> editing_values_;
    char                period_buf_[16]   = "100";

    // sin action state
    int                 sin_sig_idx_      = 0;
    double              sin_amplitude_    = 1.0;
    double              sin_offset_       = 0.0;
    int                 sin_period_ms_    = 1000;
    int                 sin_interval_ms_  = 10;
};
