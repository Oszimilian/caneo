#include "TuiDataFrameSet.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <chrono>

using namespace ftxui;

void TuiDataFrameSet::update(const CanFrame& frame) {
    {
        std::lock_guard lock(mutex_);
        const std::string& iface = frame.header().interface;
        auto it = sets_.find(iface);
        if (it == sets_.end()) {
            it = sets_.emplace(iface, DataFrameSet{iface}).first;
        }
        it->second.update(frame);
    }
    screen_.PostEvent(Event::Custom);
}

void TuiDataFrameSet::run() {
    auto renderer = Renderer([this] { return render(); });
    renderer |= CatchEvent([this](Event event) {
        if (event == Event::Character('q') || event == Event::Escape) {
            screen_.ExitLoopClosure()();
            return true;
        }
        if (event == Event::ArrowRight) {
            std::lock_guard lock(mutex_);
            if (!sets_.empty())
                selected_tab_ = (selected_tab_ + 1) % sets_.size();
            return true;
        }
        if (event == Event::ArrowLeft) {
            std::lock_guard lock(mutex_);
            if (!sets_.empty())
                selected_tab_ = (selected_tab_ + sets_.size() - 1) % sets_.size();
            return true;
        }
        return false;
    });
    screen_.Loop(renderer);
}

Element TuiDataFrameSet::render() const {
    std::lock_guard lock(mutex_);

    if (sets_.empty()) {
        return window(text(" caneo "), text("Waiting for frames...") | dim | center);
    }

    const std::size_t tab_count = sets_.size();
    const std::size_t tab_idx = selected_tab_ < tab_count ? selected_tab_ : tab_count - 1;

    // Tab bar
    Elements tabs;
    std::size_t i = 0;
    const DataFrameSet* active_set = nullptr;
    for (const auto& [iface, set] : sets_) {
        if (i == tab_idx) {
            tabs.push_back(text(" " + iface + " ") | bold | inverted);
            active_set = &set;
        } else {
            tabs.push_back(text(" " + iface + " "));
        }
        if (i + 1 < tab_count)
            tabs.push_back(text("│"));
        ++i;
    }

    // Frame list for the active tab
    Elements frame_rows;
    if (active_set) {
        for (const auto& [id, frame] : active_set->frames()) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%03X", id);
            const std::string id_str = "0x" + std::string(buf);

            std::string delta_str = " | Δ ---    ";
            if (const auto d = active_set->delta(id)) {
                const double ms = std::chrono::duration<double, std::milli>(*d).count();
                char dbuf[16];
                std::snprintf(dbuf, sizeof(dbuf), " | Δ %6.1fms", ms);
                delta_str = dbuf;
            }

            // Build hex payload string
            std::string hex_str;
            for (const uint8_t byte : frame.payload()) {
                char hbuf[4];
                std::snprintf(hbuf, sizeof(hbuf), "%02X ", byte);
                hex_str += hbuf;
            }
            if (!hex_str.empty()) hex_str.pop_back(); // trailing space

            const bool has_decoded = !frame.decoded().empty();

            Elements frame_block = {
                hbox(text(id_str) | bold,
                     text(" | DLC:" + std::to_string(frame.header().dlc)),
                     text(delta_str) | dim,
                     has_decoded ? text("") : text(" | " + hex_str) | dim),
            };
            for (const auto& sig : frame.decoded()) {
                frame_block.push_back(
                    text("    " + sig.name + ": " + std::to_string(sig.value) + " " + sig.unit));
            }
            frame_rows.push_back(vbox(std::move(frame_block)));
        }
    }
    if (frame_rows.empty())
        frame_rows.push_back(text("(no frames)") | dim);

    return window(
        text(" caneo "),
        vbox({
            hbox(std::move(tabs)),
            separator(),
            vbox(std::move(frame_rows)),
        }));
}
