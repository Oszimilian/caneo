#pragma once

#include "GuiDataFrameSet.hpp"
#include "frame/DataFrameSet.hpp"

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <cstddef>
#include <map>
#include <mutex>
#include <string>

class TuiDataFrameSet : public GuiDataFrameSet {
public:
    void update(const CanFrame& frame) override;
    void run() override;

private:
    ftxui::Element render() const;

    mutable std::mutex mutex_;
    std::map<std::string, DataFrameSet> sets_;
    std::size_t selected_tab_ = 0;
    ftxui::ScreenInteractive screen_ = ftxui::ScreenInteractive::Fullscreen();
};
