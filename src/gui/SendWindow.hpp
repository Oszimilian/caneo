#pragma once

#include "Window.hpp"
#include "action/ActionHandler.hpp"
#include "config/Config.hpp"

#include <vector>

class SendWindow : public Window {
public:
    SendWindow(const std::vector<InterfaceConfig>& iface_configs,
               ActionHandler& action_handler);

    void render() override;

private:
    ActionHandler& action_handler_;
};
