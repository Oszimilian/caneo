#pragma once

#include "Window.hpp"
#include "action/ActionHandler.hpp"

class ActionsWindow : public Window {
public:
    explicit ActionsWindow(ActionHandler& action_handler);

    void render() override;

private:
    ActionHandler& action_handler_;
};
