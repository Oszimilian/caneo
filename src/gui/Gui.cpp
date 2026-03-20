#include "Gui.hpp"

#include "ActionsWindow.hpp"
#include "GraphWindow.hpp"
#include "SendWindow.hpp"
#include "TraceWindow.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>

Gui::Gui(const std::vector<InterfaceConfig>& iface_configs,
         ActionHandler& action_handler)
{
    auto graph_window = std::make_unique<GraphWindow>();
    GraphWindow* gw = graph_window.get();

    for (const auto& cfg : iface_configs)
        windows_.push_back(std::make_unique<TraceWindow>(cfg.name, *gw));

    windows_.push_back(std::move(graph_window));

    auto send_window = std::make_unique<SendWindow>(iface_configs, action_handler);
    SendWindow* sw   = send_window.get();
    windows_.push_back(std::move(send_window));
    windows_.push_back(std::make_unique<ActionsWindow>(action_handler, *sw));
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

    GLFWwindow* window = glfwCreateWindow(3840, 2160, "caneo", nullptr, nullptr);
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
    io.FontGlobalScale = 2.5f;

    ImGui::StyleColorsLight();
    ImPlot::StyleColorsLight();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        for (auto& w : windows_)
            w->render();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.82f, 0.82f, 0.82f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}
