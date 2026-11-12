#include "Gui.hpp"

#include "DebugView.hpp"
#include "GraphWindow.hpp"
#include "FlowEditorWindow.hpp"
#include "PipelineWindow.hpp"
#include "FlowsWindow.hpp"
#include "GuiStyle.hpp"
#include "LogViewer.hpp"
#include "config/AppConfig.hpp"
#include "config/RecentConfigs.hpp"
#include "DbcEditor.hpp"
#include "XcpView.hpp"
#include "ModelWindow.hpp"
#include "SendWindow.hpp"
#include "SettingsWindow.hpp"
#include "TraceContainerWindow.hpp"
#include "TraceWindow.hpp"
#include "logger/ILogControl.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>
#include <implot.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <imgui_stdlib.h>
#include <fstream>
#include <sstream>

static constexpr const char* INI_FILE = "imgui.ini";

Gui::Gui(const std::vector<InterfaceConfig>& iface_configs, AppState& state)
    : state_(state)
{
    debug_view_ = std::make_unique<DebugView>(state_);
    dbc_editor_ = std::make_unique<DbcEditor>(state_);
    xcp_view_   = std::make_unique<XcpView>(state_);

    auto graph_window = std::make_unique<GraphWindow>();
    graph_window_ = graph_window.get();
    state_.set_graph_window(graph_window_);

    auto trace_container = std::make_unique<TraceContainerWindow>();
    for (const auto& cfg : iface_configs)
        if (cfg.live)
            trace_container->add(std::make_unique<TraceWindow>(cfg.name, *graph_window_));
    TraceContainerWindow* tc = trace_container.get();
    windows_.push_back(std::move(trace_container));

    windows_.push_back(std::move(graph_window));
    windows_.push_back(std::make_unique<SendWindow>(iface_configs, state_));

    auto flow_editor = std::make_unique<FlowEditorWindow>(state_);
    flow_window_     = flow_editor.get();
    auto flows_list  = std::make_unique<FlowsWindow>(state_, *flow_window_);
    flows_list_      = flows_list.get();
    // right-click a signal in the trace: a guard is a flow (CAN In -> Lua -> Scopes)
    tc->set_add_guard_callback([this](const std::string& iface, const std::string& msg, const std::string& sig) {
        flow_window_->create_flow(FlowsWindow::guard_template(iface, msg, sig));
        flow_window_->visible = true;
        ImGui::SetWindowFocus(flow_window_->title());
    });
    windows_.push_back(std::move(flows_list));
    windows_.push_back(std::move(flow_editor));

    pipeline_ = std::make_unique<PipelineWindow>(state_);

    auto settings = std::make_unique<SettingsWindow>(state_);
    settings->set_pipeline(pipeline_.get());
    settings_window_ = settings.get();
    windows_.push_back(std::move(settings));
}

Gui::~Gui() = default;

void Gui::stop()
{
    stop_requested_ = true;
}

void Gui::set_status_indicator(std::string label, std::function<bool()> connected_fn)
{
    status_label_        = std::move(label);
    status_connected_fn_ = std::move(connected_fn);
}

void Gui::set_log_controller(ILogControl* lc)
{
    log_controller_ = lc;
}

ModelWindow* Gui::add_model_window()
{
    auto mw = std::make_unique<ModelWindow>(*graph_window_);
    auto* ptr = mw.get();
    windows_.push_back(std::move(mw));
    return ptr;
}

void Gui::update(const CanFrame& frame)
{
    for (auto& w : windows_)
        w->update(frame);
}

// ─── layout ─────────────────────────────────────────────────────────────────

void Gui::build_default_layout(ImGuiID dockspace_id)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, vp->WorkSize);

    // ┌──────────┬────────────────┐
    // │  Trace   │                │
    // │          │     Graphs     │
    // ├──────────┤  (+ Scripts)   │
    // │ Send/Act │                │
    // └──────────┴────────────────┘
    ImGuiID left = 0, right = 0, left_bottom = 0;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.42f, &left, &right);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.40f, &left_bottom, &left);

    ImGui::DockBuilderDockWindow("Trace", left);
    ImGui::DockBuilderDockWindow("Send", left_bottom);
    ImGui::DockBuilderDockWindow("Flows", left_bottom);
    ImGui::DockBuilderDockWindow("Model", left_bottom);
    ImGui::DockBuilderDockWindow("Graphs", right);
    ImGui::DockBuilderDockWindow("Flow Editor", right);
    ImGui::DockBuilderFinish(dockspace_id);
}

static bool ini_has_dock_layout()
{
    std::ifstream f(INI_FILE);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str().find("[Docking]") != std::string::npos;
}

// ─── menu bar ───────────────────────────────────────────────────────────────

void Gui::render_menu_bar()
{
    if (!ImGui::BeginMainMenuBar())
        return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Load config..."))
            settings_window_->open_load_dialog();
        const std::string save_label = "Save config (" + state_.path() + ")";
        if (ImGui::MenuItem(save_label.c_str()))
            state_.save();
        if (ImGui::MenuItem("Save config as..."))
            settings_window_->open_save_as_dialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Settings..."))
            mode_ = Mode::Settings;
        ImGui::Separator();
        if (ImGui::MenuItem("Save & restart"))
            if (state_.save())
                state_.request_restart();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Needed to apply changed CAN channels");
        if (ImGui::MenuItem("Quit"))
            stop();
        ImGui::EndMenu();
    }

    const bool live = mode_ == Mode::Live;

    if (mode_ == Mode::Debug && ImGui::BeginMenu("Debug")) {
        if (ImGui::MenuItem("Open log..."))
            debug_view_->open_file_dialog();
        if (ImGui::BeginMenu("History")) {
            debug_view_->render_history_items();
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (mode_ == Mode::Dbc && ImGui::BeginMenu("DBC")) {
        if (ImGui::MenuItem("Import..."))
            dbc_editor_->open_file_dialog();
        if (ImGui::BeginMenu("History")) {
            dbc_editor_->render_history_items();
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (mode_ == Mode::Xcp && ImGui::BeginMenu("XCP")) {
        xcp_view_->render_menu_items();
        ImGui::EndMenu();
    }

    if (live && ImGui::BeginMenu("View")) {
        for (auto& w : windows_)
            if (w.get() != settings_window_)
                ImGui::MenuItem(w->title(), nullptr, &w->visible);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) {
            reset_layout_ = true;
            for (auto& w : windows_)
                w->visible = w.get() != settings_window_;
        }
        ImGui::EndMenu();
    }

    if (live && ImGui::BeginMenu("Flows")) {
        if (ImGui::MenuItem("New flow")) {
            flow_window_->start_new_flow();
            flow_window_->visible = true;
            ImGui::SetWindowFocus(flow_window_->title());
        }
        if (ImGui::MenuItem("Flow list"))
            flows_list_->visible = true;
        ImGui::Separator();
        ImGui::TextDisabled("Tip: right-click a signal in the trace -> Add guard");
        ImGui::EndMenu();
    }

    render_mode_toggle();
    render_status();
    ImGui::EndMainMenuBar();

    render_log_dialog();   // popups must not live inside the menu bar
}

void Gui::open_in_viewer(const std::string& file)
{
    if (const auto err = log_viewer::launch(state_.config().log_viewer, file)) {
        viewer_error_      = *err;
        viewer_error_time_ = ImGui::GetTime();
    }
}

void Gui::render_log_dialog()
{
    namespace fs = std::filesystem;

    if (open_log_dialog_) {
        open_log_dialog_ = false;
        const fs::path dir = state_.config().log_dir().value_or(fs::path{});
        fs::path name = AppConfig::log_file_name();
        if (state_.config().logging.format == "blf") name.replace_extension(".blf");
        log_path_ = (dir / name).string();
        log_error_.clear();
        ImGui::OpenPopup("Start logging");
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Start logging", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const float em = ImGui::GetFontSize();
    bool start = false;

    if (!log_controller_->supports_path()) {
        ImGui::TextUnformatted("The gRPC server decides where the log is written.");
        start = ImGui::Button("Start", ImVec2(em * 5, 0));
    } else {
        const auto& lo = state_.config().logging;
        ImGui::Text("Log file (%s)", lo.format == "both" ? "MCAP + BLF" : lo.format == "blf" ? "BLF" : "MCAP");
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(em * 32);
        if (ImGui::InputText("##logfile", &log_path_, ImGuiInputTextFlags_EnterReturnsTrue))
            start = true;
        ImGui::SameLine();
        if (ImGui::Button("..."))
            log_browser_.open("Log file", FileBrowser::Mode::Save,
                              {state_.config().logging.format == "blf" ? ".blf" : ".mcap"}, expand_path(log_path_));
        if (const auto chosen = log_browser_.render())
            log_path_ = chosen->string();

        if (lo.format == "both") {
            fs::path stem = expand_path(log_path_);
            if (stem.extension() == ".mcap" || stem.extension() == ".blf") stem.replace_extension();
            ImGui::TextDisabled("writes %s.mcap and %s.blf", stem.filename().string().c_str(),
                                stem.filename().string().c_str());
        }
        ImGui::Checkbox("Use this directory as default log directory", &remember_dir_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stored as log_file_path in the config (save the config to keep it)");

        if (!log_error_.empty())
            ImGui::TextColored(ImVec4(0.85f, 0.2f, 0.1f, 1.0f), "%s", log_error_.c_str());

        ImGui::Spacing();
        start |= ImGui::Button("Start", ImVec2(em * 5, 0));
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em * 5, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        ImGui::CloseCurrentPopup();

    if (start) {
        try {
            if (log_controller_->supports_path()) {
                if (log_path_.empty())
                    throw std::runtime_error("Please enter a file name.");
                log_controller_->start(log_path_);   // extension(s) set by the format
                if (remember_dir_) {
                    std::string dir = fs::path(log_path_).parent_path().string();
                    // keep ~ short and portable
                    if (const char* home = std::getenv("HOME"); home && dir.starts_with(std::string(home) + "/"))
                        dir = "~/" + dir.substr(std::string(home).size() + 1);
                    state_.config().log_file_path = dir.empty() ? "." : dir;
                }
            } else {
                log_controller_->start();
            }
            ImGui::CloseCurrentPopup();
        } catch (const std::exception& e) {
            log_error_ = e.what();
        }
    }
    ImGui::EndPopup();
}

void Gui::render_mode_toggle()
{
    // Two-segment switch centred in the menu bar.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float       bw    = ImGui::GetFontSize() * 4.5f;
    const float       total = bw * 5 + style.ItemSpacing.x * 4;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - total) * 0.5f);

    const auto segment = [&](const char* label, Mode m, const char* tip) {
        const bool active = mode_ == m;
        ImGui::PushStyleColor(ImGuiCol_Button, active ? style.Colors[ImGuiCol_ButtonActive]
                                                      : ImVec4(0, 0, 0, 0));
        if (ImGui::Button(label, ImVec2(bw, 0)))
            mode_ = m;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tip);
    };
    segment("Live", Mode::Live, "Live CAN traffic: trace, graphs, send, actions");
    ImGui::SameLine();
    segment("Debug", Mode::Debug, "Offline: inspect MCAP / BLF log files");
    ImGui::SameLine();
    segment("DBC", Mode::Dbc, "Create and edit DBC files: nodes, messages, signals, value tables");
    ImGui::SameLine();
    segment("XCP", Mode::Xcp, "Measure and calibrate an ECU over XCP on CAN (FD): ELF variables, live plots, edit values");
    ImGui::SameLine();
    segment("Settings", Mode::Settings, "Appearance, configuration file, logging, channels");
}

void Gui::set_channel_control(std::function<ChannelResult(bool)> start, std::function<void()> stop)
{
    channel_start_ = std::move(start);
    channel_stop_  = std::move(stop);
}

Gui::ChannelResult Gui::start_channels_now(bool graphical)
{
    if (!channel_start_) return {};
    const ChannelResult res = channel_start_(graphical);
    channels_running_ = res.running;
    return res;
}

// Start / Stop: the interfaces are only set up (password!) and opened when the user asks for it.
void Gui::render_channel_button()
{
    if (!channel_start_) return;

    if (starter_.joinable() && !channels_starting_) {   // the start thread finished
        starter_.join();
        channels_running_ = start_result_.running;
        state_.set_status(start_result_.message, !start_result_.running || start_result_.message.find('\n') != std::string::npos);
    }

    if (channels_starting_) {
        ImGui::BeginDisabled();
        ImGui::SmallButton("Starting...");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Setting up the CAN interfaces - enter the password if asked");
    } else if (channels_running_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.2f, 0.15f, 1.0f));
        if (ImGui::SmallButton("Stop")) {
            channel_stop_();
            channels_running_ = false;
            state_.set_status("Channels stopped");
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Closes all CAN channels");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.2f, 1.0f));
        if (ImGui::SmallButton("Start")) {
            channels_starting_ = true;
            starter_ = std::jthread([this] {
                start_result_     = channel_start_(true);
                channels_starting_ = false;
            });
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Sets up the CAN interfaces (asks for the password once, only if needed) and opens the channels");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
}

// First start without a config file: pick one from the history, browse for one, or go on with a blank config.
void Gui::render_welcome()
{
    if (search_config_) {
        search_config_ = false;
        recent_ = recent_configs();
        ImGui::OpenPopup("Select configuration");
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 42, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Select configuration", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const float em = ImGui::GetFontSize();
        ImGui::TextWrapped("No caneo.yaml in %s.", std::filesystem::current_path().string().c_str());
        ImGui::Spacing();

        ImGui::TextUnformatted("Recently used");
        if (recent_.empty())
            ImGui::TextDisabled("(none yet)");
        std::string chosen;
        if (!recent_.empty() && ImGui::BeginChild("##recent", ImVec2(0, std::min<float>(recent_.size(), 8) * ImGui::GetFrameHeightWithSpacing() + 4),
                                                  ImGuiChildFlags_Border)) {
            for (const auto& r : recent_) {
                const std::filesystem::path p(r);
                const std::string label = p.filename().string() + "   " + p.parent_path().string();
                if (ImGui::Selectable(label.c_str()))
                    chosen = r;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", r.c_str());
            }
        }
        if (!recent_.empty()) ImGui::EndChild();

        ImGui::Spacing();
        if (ImGui::Button("Browse...", ImVec2(em * 8, 0)))
            welcome_browser_.open("Select config", FileBrowser::Mode::Open, {".yaml", ".yml"}, std::filesystem::current_path());
        ImGui::SameLine();
        if (ImGui::Button("Blank config", ImVec2(em * 8, 0)))
            ImGui::CloseCurrentPopup();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Start empty; Save writes %s", state_.path().c_str());

        if (const auto picked = welcome_browser_.render())
            chosen = picked->string();

        if (!chosen.empty()) {
            if (state_.load(chosen)) {
                state_.request_restart();   // channels, DBCs ... are built from the config at start-up
                ImGui::CloseCurrentPopup();
            }
        }
        if (state_.status_error())
            ImGui::TextColored(ImVec4(0.85f, 0.2f, 0.1f, 1.0f), "%s", state_.status().c_str());
        ImGui::EndPopup();
    }
}

void Gui::render_status()
{
    // Right-aligned using the width measured in the previous frame.
    const float avail = ImGui::GetContentRegionAvail().x;
    if (status_width_ > 0.0f && status_width_ < avail)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - status_width_ - ImGui::GetStyle().ItemSpacing.x);

    ImGui::BeginGroup();

    render_channel_button();

    if (state_.channels_changed()) {
        ImGui::TextColored(ImVec4(0.85f, 0.5f, 0.05f, 1.0f), "restart required");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("CAN channel settings were changed - File > Save & restart");
        if (ImGui::IsItemClicked())
            mode_ = Mode::Settings;
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
    }

    if (log_controller_) {
        const bool logging = log_controller_->is_logging();
        if (logging) {
            const double secs = log_controller_->elapsed_seconds();
            const int    h    = static_cast<int>(secs) / 3600;
            const int    m    = (static_cast<int>(secs) % 3600) / 60;
            const int    s    = static_cast<int>(secs) % 60;
            ImGui::TextColored(ImVec4(0.85f, 0.1f, 0.1f, 1.0f), "REC %02d:%02d:%02d", h, m, s);
            if (ImGui::IsItemHovered()) {
                const std::string path = log_controller_->current_path();
                ImGui::SetTooltip("%s\n(click to copy the path)", path.c_str());
            }
            if (ImGui::IsItemClicked())
                ImGui::SetClipboardText(log_controller_->current_path().c_str());
        } else {
            ImGui::TextDisabled("not logging");
        }
        ImGui::SameLine();
        if (logging) {
            if (ImGui::SmallButton("Stop log"))
                log_controller_->stop();
        } else {
            if (ImGui::SmallButton("Start log..."))
                open_log_dialog_ = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Choose the file and start logging");
        }

        // Open the last finished log in the configured viewer (PlotJuggler, ...)
        if (log_controller_->supports_path()) {
            const std::string& cmd  = state_.config().log_viewer;
            std::string        low  = cmd;
            std::ranges::transform(low, low.begin(), [](unsigned char c) { return std::tolower(c); });
            const char*        name = low.find("plotjuggler") != std::string::npos ? "PlotJuggler" : "View log";
            const std::string  last = log_controller_->last_path();
            ImGui::SameLine();
            if (ImGui::SmallButton(name)) {
                if (cmd.empty()) mode_ = Mode::Settings;
                else             open_in_viewer(last);
            }
            if (ImGui::IsItemHovered()) {
                if (cmd.empty())
                    ImGui::SetTooltip("No viewer configured - set one in Settings > Logging");
                else if (last.empty())
                    ImGui::SetTooltip("Start the viewer (no finished log yet)");
                else
                    ImGui::SetTooltip("Open %s%s", last.c_str(),
                                      logging ? "\n(last finished log - the running one is still being written)" : "");
            }
        }
    }

    if (!viewer_error_.empty() && ImGui::GetTime() - viewer_error_time_ < 6.0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.85f, 0.2f, 0.1f, 1.0f), "%s", viewer_error_.c_str());
    }

    if (status_connected_fn_) {
        if (log_controller_) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
        }
        const bool connected = status_connected_fn_();
        ImGui::SameLine();
        ImGui::TextColored(connected ? ImVec4(0.1f, 0.7f, 0.15f, 1.0f) : ImVec4(0.85f, 0.2f, 0.1f, 1.0f),
                           connected ? "[OK]" : "[!!]");
        ImGui::SameLine();
        ImGui::Text("%s%s", status_label_.c_str(), connected ? "" : " (reconnecting...)");
    }

    ImGui::EndGroup();
    status_width_ = ImGui::GetItemRectSize().x;
}

// ─── main loop ──────────────────────────────────────────────────────────────

void Gui::run()
{
    if (!glfwInit())
        return;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);

    // Use primary monitor resolution as initial window size so GLFW has a
    // sensible fallback even before the maximise hint takes effect.
    GLFWmonitor*       primary   = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode      = primary ? glfwGetVideoMode(primary) : nullptr;
    int                init_w    = mode ? mode->width  : 1280;
    int                init_h    = mode ? mode->height : 720;

    GLFWwindow* window = glfwCreateWindow(init_w, init_h, "caneo", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename  = INI_FILE;   // window positions + dock layout
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // A layout from before docking (or none at all) gets the default layout.
    reset_layout_ = !ini_has_dock_layout();

    LogOptions  applied_log_options = state_.config().logging;
    float       applied_scale = state_.config().gui.scale;
    std::string applied_theme = state_.config().gui.theme;
    gui_style::apply(applied_scale, applied_theme, false);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");


    // Start-up problems (missing interface, broken DBC) are shown in Settings.
    if (state_.status_error())
        mode_ = Mode::Settings;

    while (!glfwWindowShouldClose(window) && !stop_requested_ && !state_.restart_requested()) {
        glfwPollEvents();

        state_.autosave_tick(ImGui::GetTime());

        // Logging options from Settings apply to the running log right away.
        if (log_controller_ && state_.config().logging != applied_log_options) {
            applied_log_options = state_.config().logging;
            log_controller_->set_options(applied_log_options);
        }

        // Scale / theme changes from Settings or a loaded config.
        const auto& gui_cfg = state_.config().gui;
        if (gui_cfg.scale != applied_scale || gui_cfg.theme != applied_theme) {
            applied_scale = gui_cfg.scale;
            applied_theme = gui_cfg.theme;
            gui_style::apply(applied_scale, applied_theme, true);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        render_menu_bar();   // before the dockspace so it reserves its space
        render_welcome();

        if (mode_ == Mode::Debug) {
            debug_view_->render();
        } else if (mode_ == Mode::Dbc) {
            dbc_editor_->render();
        } else if (mode_ == Mode::Xcp) {
            xcp_view_->render();
        } else if (mode_ == Mode::Settings) {
            settings_window_->render_full();
        } else {
            const ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(ImGui::GetMainViewport());
            if (reset_layout_) {
                build_default_layout(dockspace_id);
                reset_layout_ = false;
            }

            for (auto& w : windows_)
                if (w->visible)
                    w->render();
        }

        // Load/save dialogs of the File menu live in the settings window but
        // must be drawn even while it is closed.
        settings_window_->render_dialogs();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        glClearColor(bg.x * 0.9f, bg.y * 0.9f, bg.z * 0.9f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    xcp_view_->shutdown();   // its worker logs through objects of run_gui

    // Save settings explicitly so they're not lost on signal shutdown.
    ImGui::SaveIniSettingsToDisk(io.IniFilename);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}
