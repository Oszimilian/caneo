#include "SettingsWindow.hpp"

#include <algorithm>
#include <cstdlib>
#include <array>
#include <tuple>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <imgui.h>
#include <imgui_stdlib.h>

#include "LogViewer.hpp"

namespace fs = std::filesystem;

static constexpr int ARPHRD_CAN_TYPE = 280;   // <linux/if_arp.h> ARPHRD_CAN

static const std::array<std::pair<const char*, uint32_t>, 7> BAUDRATES = {{
    {"125 kbit/s", 125'000}, {"250 kbit/s", 250'000}, {"500 kbit/s", 500'000},
    {"800 kbit/s", 800'000}, {"1 Mbit/s", 1'000'000}, {"(not set)", 0}, {"Custom", 1},
}};

SettingsWindow::SettingsWindow(AppState& state)
    : state_(state)
{
    visible = false;
}

void SettingsWindow::refresh_detected()
{
    detected_.clear();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/net", ec)) {
        int type = 0;
        std::ifstream(entry.path() / "type") >> type;
        if (type != ARPHRD_CAN_TYPE)
            continue;
        DetectedIface d;
        d.name = entry.path().filename().string();
        std::ifstream(entry.path() / "operstate") >> d.state;
        // vcan devices have no backing device
        d.is_virtual = !fs::exists(entry.path() / "device", ec);
        detected_.push_back(std::move(d));
    }
    std::ranges::sort(detected_, {}, &DetectedIface::name);
    detected_time_ = ImGui::GetTime();
}

void SettingsWindow::open_load_dialog()
{
    dialog_ = Dialog::Load;
    browser_.open("Load configuration", FileBrowser::Mode::Open, {".yaml", ".yml"}, state_.path());
}

void SettingsWindow::open_save_as_dialog()
{
    dialog_ = Dialog::SaveAs;
    browser_.open("Save configuration as", FileBrowser::Mode::Save, {".yaml", ".yml"}, state_.path());
}

void SettingsWindow::render_dialogs()
{
    const auto chosen = browser_.render();
    if (!browser_.is_open() && !chosen)
        dialog_ = Dialog::None;
    if (!chosen)
        return;

    switch (dialog_) {
        case Dialog::Dbc: {
            auto& ifaces = state_.config().interfaces;
            if (dialog_row_ >= 0 && dialog_row_ < static_cast<int>(ifaces.size()))
                ifaces[dialog_row_].dbc = chosen->string();
            break;
        }
        case Dialog::ViewerExe: {
            // Keep the arguments of an existing PlotJuggler-style command.
            std::string exe = chosen->string();
            if (exe.find(' ') != std::string::npos) exe = "'" + exe + "'";
            std::string low = exe;
            std::ranges::transform(low, low.begin(), [](unsigned char c) { return std::tolower(c); });
            state_.config().log_viewer = exe + (low.find("plotjuggler") != std::string::npos ? " -n -d {file}" : " {file}");
            break;
        }
        case Dialog::OpenLog:
            if (auto err = log_viewer::launch(state_.config().log_viewer, chosen->string()))
                viewer_error_ = *err;
            else
                viewer_error_.clear();
            break;
        case Dialog::Load:   state_.load(chosen->string()); break;
        case Dialog::SaveAs: state_.save(chosen->string()); break;
        case Dialog::None:   break;
    }
    dialog_ = Dialog::None;
}

// ─── sections ───────────────────────────────────────────────────────────────

void SettingsWindow::render_appearance()
{
    auto& gui = state_.config().gui;
    if (!scale_dragging_)
        scale_edit_ = gui.scale * 100.0f;

    ImGui::TextUnformatted("GUI scale");
    ImGui::SameLine(ImGui::GetFontSize() * 7);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::SliderFloat("##scale", &scale_edit_, 50.0f, 300.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
    scale_dragging_ = ImGui::IsItemActive();
    // Rebuilding the fonts is expensive: apply when the slider is released.
    if (ImGui::IsItemDeactivatedAfterEdit())
        gui.scale = std::round(scale_edit_) / 100.0f;

    for (const float preset : {100.0f, 125.0f, 150.0f, 200.0f}) {
        ImGui::SameLine();
        const std::string label = std::format("{:.0f}%", preset);
        if (ImGui::Button(label.c_str()))
            gui.scale = preset / 100.0f;
    }

    ImGui::TextUnformatted("Theme");
    ImGui::SameLine(ImGui::GetFontSize() * 7);
    if (ImGui::RadioButton("Light", gui.theme != "dark")) gui.theme = "light";
    ImGui::SameLine();
    if (ImGui::RadioButton("Dark", gui.theme == "dark"))  gui.theme = "dark";
}

void SettingsWindow::render_config_file()
{
    ImGui::TextUnformatted("File");
    ImGui::SameLine(ImGui::GetFontSize() * 7);
    ImGui::TextUnformatted(state_.path().c_str());

    if (ImGui::Button("Load..."))
        open_load_dialog();
    ImGui::SameLine();
    if (ImGui::Button("Save"))
        state_.save();
    ImGui::SameLine();
    if (ImGui::Button("Save as..."))
        open_save_as_dialog();
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Saves channels, GUI settings, all flows and all graphs\n"
                          "into one YAML file. Comments in the file are not kept.");

    auto& gui = state_.config().gui;
    bool autosave = gui.autosave_s > 0;
    if (ImGui::Checkbox("Auto-save every", &autosave))
        gui.autosave_s = autosave ? 10 : 0;
    ImGui::SameLine();
    ImGui::BeginDisabled(!autosave);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    int secs = autosave ? gui.autosave_s : 10;
    if (ImGui::InputInt("s##autosave", &secs, 5, 30) && autosave)
        gui.autosave_s = std::clamp(secs, 1, 3600);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Writes the current state (channels, flows, graphs, settings)\n"
                          "to the config file whenever something changed. Comments are not kept.");

    if (!state_.status().empty()) {
        if (state_.status_error())
            ImGui::TextColored(ImVec4(0.85f, 0.2f, 0.1f, 1.0f), "%s", state_.status().c_str());
        else
            ImGui::TextDisabled("%s", state_.status().c_str());
    }
    if (!state_.warnings().empty() && ImGui::TreeNode("warnings", "%zu warning(s)", state_.warnings().size())) {
        for (const auto& w : state_.warnings())
            ImGui::BulletText("%s", w.c_str());
        ImGui::TreePop();
    }
}

void SettingsWindow::render_channels()
{
    auto& cfg = state_.config();

    if (detected_time_ < 0.0 || ImGui::GetTime() - detected_time_ > 2.0)
        refresh_detected();

    ImGui::Checkbox("Virtual CAN (vcan)", &cfg.virtual_can);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On start-up, load the vcan module and create the\n"
                          "interfaces below as virtual CAN devices (needs sudo).");

    ImGui::Spacing();

    constexpr ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp;
    const float em = ImGui::GetFontSize();
    int remove_row = -1;

    if (ImGui::BeginTable("##channels", 4, tflags)) {
        ImGui::TableSetupColumn("Interface", ImGuiTableColumnFlags_WidthFixed, em * 9);
        ImGui::TableSetupColumn("Baudrate",  ImGuiTableColumnFlags_WidthFixed, em * 9);
        ImGui::TableSetupColumn("DBC file",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("",          ImGuiTableColumnFlags_WidthFixed, em * 1.8f);
        ImGui::TableHeadersRow();

        for (int row = 0; row < static_cast<int>(cfg.interfaces.size()); ++row) {
            auto& iface = cfg.interfaces[row];
            ImGui::PushID(row);
            ImGui::TableNextRow();

            // Interface: free text + drop-down of detected CAN devices
            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-ImGui::GetFrameHeight());
            ImGui::InputText("##name", &iface.name, ImGuiInputTextFlags_CharsNoBlank);
            ImGui::SameLine(0, 0);
            if (ImGui::BeginCombo("##detected", nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft)) {
                if (detected_.empty())
                    ImGui::TextDisabled("No CAN interfaces found");
                for (const auto& d : detected_) {
                    const std::string label = std::format("{}  ({}{})", d.name, d.state, d.is_virtual ? ", virtual" : "");
                    if (ImGui::Selectable(label.c_str(), d.name == iface.name))
                        iface.name = d.name;
                }
                ImGui::EndCombo();
            }
            const bool exists = std::ranges::any_of(detected_, [&](const auto& d) { return d.name == iface.name; });
            if (!exists && !cfg.virtual_can && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s does not exist on this machine", iface.name.c_str());

            // Baudrate
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginDisabled(cfg.virtual_can);
            const char* preview = "Custom";
            for (const auto& [label, rate] : BAUDRATES)
                if (rate == iface.baudrate) preview = label;
            const bool custom = std::string_view(preview) == "Custom";
            ImGui::SetNextItemWidth(custom ? em * 4.5f : -1);
            if (ImGui::BeginCombo("##baud", custom ? "Custom" : preview)) {
                for (const auto& [label, rate] : BAUDRATES) {
                    const bool is_custom = rate == 1;
                    if (ImGui::Selectable(label, is_custom ? custom : rate == iface.baudrate))
                        iface.baudrate = is_custom ? (custom ? iface.baudrate : 100'000) : rate;
                }
                ImGui::EndCombo();
            }
            if (custom) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputScalar("##baud_custom", ImGuiDataType_U32, &iface.baudrate);
            }
            ImGui::EndDisabled();

            // DBC
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-em * 2.5f);
            ImGui::InputTextWithHint("##dbc", "(no DBC - raw frames only)", &iface.dbc);
            if (!iface.dbc.empty() && !fs::exists(expand_path(iface.dbc)) && ImGui::IsItemHovered())
                ImGui::SetTooltip("File not found");
            ImGui::SameLine();
            if (ImGui::Button("...")) {
                dialog_     = Dialog::Dbc;
                dialog_row_ = row;
                browser_.open("Select DBC for " + iface.name, FileBrowser::Mode::Open, {".dbc"},
                              iface.dbc.empty() ? fs::path{} : expand_path(iface.dbc));
            }

            ImGui::TableSetColumnIndex(3);
            if (ImGui::Button("x"))
                remove_row = row;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Remove channel");

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (remove_row >= 0)
        cfg.interfaces.erase(cfg.interfaces.begin() + remove_row);

    if (ImGui::Button("+ Add channel")) {
        InterfaceConfig ic;
        // Suggest the first detected interface that is not configured yet.
        for (const auto& d : detected_)
            if (std::ranges::none_of(cfg.interfaces, [&](const auto& c) { return c.name == d.name; })) {
                ic.name = d.name;
                break;
            }
        if (ic.name.empty())
            ic.name = std::format("{}{}", cfg.virtual_can ? "vcan" : "can", cfg.interfaces.size());
        ic.baudrate = cfg.virtual_can ? 0 : 500'000;
        cfg.interfaces.push_back(std::move(ic));
    }

    if (state_.channels_changed()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.0f, 0.85f, 0.4f, 0.35f));
        if (ImGui::BeginChild("##restart", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Border)) {
            ImGui::TextWrapped("Channel changes take effect after a restart of caneo. "
                               "\"Save & Restart\" writes %s and restarts with it.", state_.path().c_str());
            if (ImGui::Button("Save & Restart") && state_.save())
                state_.request_restart();
            ImGui::SameLine();
            if (ImGui::Button("Revert")) {
                const auto& run = state_.running();
                cfg.virtual_can = run.virtual_can;
                cfg.interfaces  = run.interfaces;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

void SettingsWindow::render_logging()
{
    auto& cfg = state_.config();
    const float em = ImGui::GetFontSize();

    ImGui::TextUnformatted("Log directory");
    ImGui::SameLine(em * 7);
    ImGui::SetNextItemWidth(em * 24);
    ImGui::InputTextWithHint("##logdir", "(working directory)", &cfg.log_file_path);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Default directory offered by \"Start log...\" (~ allowed)");

    // ── what goes into the log ──
    auto& lo = cfg.logging;
    ImGui::TextUnformatted("Format");
    ImGui::SameLine(em * 7);
    for (const auto& [value, label, tip] : {
             std::tuple{"mcap", "MCAP", "Decoded signals + guard outputs. Open with PlotJuggler / Foxglove."},
             std::tuple{"blf",  "BLF",  "Raw CAN frames only (Vector BLF), very small.\n"
                                        "Open with CANalyzer/CANoe or asammdf (+ DBC). Replay: --playback file.blf"},
             std::tuple{"both", "MCAP + BLF", "Both files with the same name: BLF as the complete raw archive,\n"
                                              "MCAP (optionally thinned out) for a quick look in PlotJuggler."} }) {
        if (ImGui::RadioButton(label, lo.format == value))
            lo.format = value;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tip);
        ImGui::SameLine();
    }
    ImGui::NewLine();

    ImGui::TextUnformatted("MCAP content");
    ImGui::SameLine(em * 7);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(!lo.mcap());
    ImGui::BeginDisabled();
    bool decoded = true;
    ImGui::Checkbox("Decoded signals (<iface>/<msg>/data)", &decoded);
    ImGui::EndDisabled();
    ImGui::Checkbox("Raw bytes (<iface>/<msg>/raw)", &lo.raw);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Original CAN payload. Needed to replay the log with --playback.");
    ImGui::Checkbox("Update rate (<iface>/<msg>/update_ratio)", &lo.update_ratio);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Time since the previous frame of the same ID, in ms.");

    bool thin = lo.min_interval_ms > 0;
    if (ImGui::Checkbox("Thin out fast messages: at most every", &thin))
        lo.min_interval_ms = thin ? 10 : 0;
    ImGui::SameLine();
    ImGui::BeginDisabled(!thin);
    ImGui::SetNextItemWidth(em * 6);
    int ms = thin ? lo.min_interval_ms : 10;
    if (ImGui::InputInt("ms per ID", &ms, 1, 10) && thin)
        lo.min_interval_ms = std::clamp(ms, 1, 60'000);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Logs each CAN ID at most every N ms (e.g. 10 ms = 100 Hz).\n"
                          "Frames in between are dropped from the log (not from the trace).");

    ImGui::EndDisabled();   // MCAP-only options
    bool flush = lo.flush_s > 0;
    if (ImGui::Checkbox("Write to disk every", &flush))
        lo.flush_s = flush ? 10 : 0;
    ImGui::SameLine();
    ImGui::BeginDisabled(!flush);
    ImGui::SetNextItemWidth(em * 6);
    int fs_ = flush ? lo.flush_s : 10;
    if (ImGui::InputInt("s##flush", &fs_, 5, 30) && flush)
        lo.flush_s = std::clamp(fs_, 1, 3600);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Flushes the log so at most this much data is lost on a crash or power cut.\n"
                          "A file that was not stopped cleanly can still be opened (fallback scan).");


    // rough size estimate per frame (measured, see README "Log size")
    const int    channels  = 1 + (lo.raw ? 1 : 0) + (lo.update_ratio ? 1 : 0);
    double       per_frame = 0.0;
    if (lo.mcap()) per_frame += channels * 16.0 + 6.0 + (lo.raw ? 9.0 : 0.0) + (lo.update_ratio ? 9.0 : 0.0);
    if (lo.blf())  per_frame += 11.0;
    ImGui::TextDisabled("approx. %.0f bytes per frame%s  (~%.2f GB/h at 4000 frames/s)",
                        per_frame, lo.min_interval_ms > 0 && lo.mcap() ? " before thinning" : "",
                        per_frame * 4000.0 * 3600.0 / 1e9);
    ImGui::TextDisabled("Format changes apply to the next log.");
    ImGui::EndGroup();
    ImGui::Spacing();

    ImGui::TextUnformatted("Viewer");
    ImGui::SameLine(em * 7);
    ImGui::SetNextItemWidth(-em * 2.5f);
    ImGui::InputTextWithHint("##viewer", "e.g. ~/Downloads/PlotJuggler.AppImage -n -d {file}", &cfg.log_viewer);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Command to open a log file. {file} is replaced by the path of the .mcap\n"
                          "(without {file} the path is appended). Runs via /bin/sh, ~ works.");
    ImGui::SameLine();
    if (ImGui::Button("...##viewer_exe")) {
        dialog_ = Dialog::ViewerExe;
        browser_.open("Select viewer program", FileBrowser::Mode::Open, {},
                      std::getenv("HOME") ? fs::path(std::getenv("HOME")) / "Downloads" : fs::path{});
    }

    if (!viewer_detected_) {
        detected_viewer_ = log_viewer::detect_plotjuggler();
        viewer_detected_ = true;
    }
    if (detected_viewer_ && cfg.log_viewer != *detected_viewer_) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + em * 7);
        ImGui::TextDisabled("Found: %s", detected_viewer_->c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Use"))
            cfg.log_viewer = *detected_viewer_;
    }

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + em * 7);
    ImGui::BeginDisabled(cfg.log_viewer.empty());
    if (ImGui::Button("Open log file...")) {
        dialog_ = Dialog::OpenLog;
        const fs::path dir = cfg.log_dir().value_or(fs::path{});
        browser_.open("Open log in viewer", FileBrowser::Mode::Open, {".mcap"}, dir);
    }
    ImGui::SameLine();
    if (ImGui::Button("Start viewer")) {
        if (auto err = log_viewer::launch(cfg.log_viewer, {})) viewer_error_ = *err;
        else                                                     viewer_error_.clear();
    }
    ImGui::EndDisabled();
    if (!viewer_error_.empty())
        ImGui::TextColored(ImVec4(0.85f, 0.2f, 0.1f, 1.0f), "%s", viewer_error_.c_str());
}

void SettingsWindow::render()
{
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 48, ImGui::GetFontSize() * 34), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin(title(), &visible)) {
        if (ImGui::CollapsingHeader("Appearance", ImGuiTreeNodeFlags_DefaultOpen))
            render_appearance();
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Configuration file", ImGuiTreeNodeFlags_DefaultOpen))
            render_config_file();
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Logging", ImGuiTreeNodeFlags_DefaultOpen))
            render_logging();
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("CAN channels", ImGuiTreeNodeFlags_DefaultOpen))
            render_channels();
    }
    ImGui::End();
}
