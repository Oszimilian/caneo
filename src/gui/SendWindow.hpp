#pragma once

#include "AppState.hpp"
#include "Window.hpp"
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

    SendWindow(const std::vector<InterfaceConfig>& iface_configs, AppState& state);

    const char* title() const override { return "Send"; }
    void render() override;

private:
    void open_popup(int iface_idx, int msg_idx);
    void render_edit_window();

    AppState&              state_;
    std::string            error_;
    std::vector<IfaceData> ifaces_;

    // edit window state
    bool                popup_open_  = false;
    int                 popup_iface_ = -1;
    int                 popup_msg_   = -1;
    std::vector<double> editing_values_;
    char                period_buf_[16]   = "100";
    std::string         info_;
};
