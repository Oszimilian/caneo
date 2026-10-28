#pragma once

#include "GraphWindow.hpp"
#include "Window.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class ModelWindow : public Window {
public:
    explicit ModelWindow(GraphWindow& graph_window);

    // Thread-safe: called from asio thread for each named model output.
    void push(const std::string& name, double value);

    const char* title() const override { return "Model"; }
    void render() override;

private:
    static constexpr const char* IFACE = GraphWindow::MODEL_IFACE;

    GraphWindow*                               graph_window_;
    mutable std::mutex                         mutex_;
    std::vector<std::pair<std::string,double>> values_; // insertion-ordered, updated in place

    std::optional<std::string> ctx_name_;
    bool                       open_ctx_popup_ = false;
};
