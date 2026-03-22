#include "Gui.hpp"

#include "ActionsWindow.hpp"
#include "GraphWindow.hpp"
#include "ModelWindow.hpp"
#include "SendWindow.hpp"
#include "TraceContainerWindow.hpp"
#include "TraceWindow.hpp"
#include "logger/ILogControl.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>
#include <implot.h>

#include <cstring>

Gui::Gui(const std::vector<InterfaceConfig>& iface_configs,
         ActionHandler& action_handler)
{
    auto graph_window = std::make_unique<GraphWindow>();
    graph_window_ = graph_window.get();

    auto trace_container = std::make_unique<TraceContainerWindow>();
    for (const auto& cfg : iface_configs)
        trace_container->add(std::make_unique<TraceWindow>(cfg.name, *graph_window_));
    windows_.push_back(std::move(trace_container));

    windows_.push_back(std::move(graph_window));

    auto send_window = std::make_unique<SendWindow>(iface_configs, action_handler);
    SendWindow* sw   = send_window.get();
    windows_.push_back(std::move(send_window));
    windows_.push_back(std::make_unique<ActionsWindow>(action_handler, *sw));
}

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

    // --- Custom ini handler: persists [caneo] section with FontGlobalScale ---
    static float s_font_scale = 1.0f;

    ImGuiSettingsHandler caneo_handler{};
    caneo_handler.TypeName   = "caneo";
    caneo_handler.TypeHash   = ImHashStr("caneo");
    caneo_handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char*) -> void* {
        return (void*)1; // any non-null = "I handle this entry"
    };
    caneo_handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line) {
        float v{};
        if (std::sscanf(line, "FontGlobalScale=%f", &v) == 1)
            s_font_scale = v;
    };
    caneo_handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
        buf->appendf("[%s][settings]\n", handler->TypeName);
        buf->appendf("FontGlobalScale=%.2f\n\n", ImGui::GetIO().FontGlobalScale);
    };
    ImGui::AddSettingsHandler(&caneo_handler);

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "imgui.ini"; // ensure file is loaded/saved

    // Load ini now so the handler above can populate s_font_scale before we use it.
    ImGui::LoadIniSettingsFromDisk(io.IniFilename);
    io.FontGlobalScale = s_font_scale;

    ImGui::StyleColorsLight();
    ImPlot::StyleColorsLight();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    while (!glfwWindowShouldClose(window) && !stop_requested_) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        for (auto& w : windows_)
            w->render();

        if (status_connected_fn_ || log_controller_) {
            const ImGuiIO& imgui_io = ImGui::GetIO();
            ImGui::SetNextWindowPos(
                ImVec2(imgui_io.DisplaySize.x - 10.0f, 10.0f),
                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
            ImGui::SetNextWindowBgAlpha(0.75f);
            ImGui::Begin("##overlay", nullptr,
                ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoNav         | ImGuiWindowFlags_NoMove  |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);

            // ---- log controls (left part of the single row) ----
            bool logging = false;
            if (log_controller_) {
                logging = log_controller_->is_logging();
                if (ImGui::Button(logging ? "Stop Log" : "Start Log")) {
                    if (logging) log_controller_->stop();
                    else         log_controller_->start();
                }
                ImGui::SameLine();
                const ImVec4 rec_color = logging
                    ? ImVec4(0.85f, 0.1f, 0.1f, 1.0f)
                    : ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
                ImGui::TextColored(rec_color, logging ? "[REC]" : "[---]");
                if (status_connected_fn_) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                }
            }

            // ---- gRPC status (right part of the single row) ----
            if (status_connected_fn_) {
                const bool connected = status_connected_fn_();
                ImGui::SameLine();
                const ImVec4 color = connected
                    ? ImVec4(0.1f, 0.7f, 0.15f, 1.0f)
                    : ImVec4(0.85f, 0.2f, 0.1f, 1.0f);
                ImGui::TextColored(color, connected ? "[OK]" : "[!!]");
                ImGui::SameLine();
                ImGui::Text("%s", status_label_.c_str());
                if (!connected) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(reconnecting...)");
                }
            }

            // ---- log file path + duration (second line, only when active) ----
            if (logging) {
                const double secs = log_controller_->elapsed_seconds();
                const int    h    = static_cast<int>(secs) / 3600;
                const int    m    = (static_cast<int>(secs) % 3600) / 60;
                const int    s    = static_cast<int>(secs) % 60;
                if (h > 0)
                    ImGui::TextDisabled("%02d:%02d:%02d", h, m, s);
                else
                    ImGui::TextDisabled("%02d:%02d", m, s);
                ImGui::SameLine();
                std::string path = log_controller_->current_path();
                ImGui::SetNextItemWidth(ImGui::CalcTextSize(path.c_str()).x + ImGui::GetStyle().FramePadding.x * 2);
                ImGui::InputText("##logpath", path.data(), path.size() + 1,
                                 ImGuiInputTextFlags_ReadOnly);
            }

            ImGui::End();
        }

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.82f, 0.82f, 0.82f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Save settings explicitly so they're not lost on signal shutdown.
    ImGui::SaveIniSettingsToDisk(io.IniFilename);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}
