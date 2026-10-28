#pragma once

#include "AppState.hpp"
#include "Window.hpp"

#include <format>
#include <ostream>
#include <sstream>

class FlowEditorWindow;

// "Flows" (left side): all live flows with their state. A click opens the flow in the Flow Editor.
// Guards and saved actions of older configs are converted to flows on load, so this is the one
// place for them.
class FlowsWindow : public Window {
public:
    FlowsWindow(AppState& state, FlowEditorWindow& editor) : state_(state), editor_(editor) {}

    const char* title() const override { return "Flows"; }
    void render() override;

    // Diagram of a guard: CAN In (signal) -> Lua check -> Scopes.
    static FlowSpec guard_template(const std::string& iface, const std::string& msg, const std::string& signal);

    friend std::ostream& operator<<(std::ostream& os, const FlowsWindow&) { return os << "FlowsWindow"; }

private:
    AppState&         state_;
    FlowEditorWindow& editor_;
};

template <>
struct std::formatter<FlowsWindow> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const FlowsWindow& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
