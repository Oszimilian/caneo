#pragma once

#include "AppState.hpp"
#include "FileBrowser.hpp"
#include "GraphWindow.hpp"
#include "Window.hpp"
#include "config/Config.hpp"
#include "frame/CanFrame.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class DebugView;
class FlowEditorWindow;
class ModelWindow;
class DbcEditor;
class ILogControl;
class SettingsWindow;

class Gui {
public:
    // iface_configs: the running channels (one trace tab each).
    // Live = the normal windows; Debug = full-screen offline log viewer.
    enum class Mode { Live, Debug, Dbc };

    Gui(const std::vector<InterfaceConfig>& iface_configs, AppState& state);
    ~Gui();

    // Thread-safe: called from asio thread, delegates to all windows.
    void update(const CanFrame& frame);

    // Blocks until window is closed; call from main thread.
    void run();

    // Thread-safe: signals the render loop to exit cleanly.
    void stop();

    // Creates a ModelWindow, adds it to the render loop, returns the raw pointer.
    // Must be called before run(). The window is owned by Gui.
    ModelWindow* add_model_window();

    // Returns the shared GraphWindow. Valid for the lifetime of this Gui.
    GraphWindow* graph_window() { return graph_window_; }

    // Shows a connection status indicator in the menu bar.
    // connected_fn is called each frame from the render thread.
    void set_status_indicator(std::string label, std::function<bool()> connected_fn);

    // Attaches a LogController so the menu bar shows a start/stop button and
    // the current log file path. May be called before run().
    void set_log_controller(ILogControl* lc);

private:
    void render_menu_bar();
    void render_mode_toggle(); // centre of the menu bar
    void render_status(); // right side of the menu bar
    void build_default_layout(unsigned int dockspace_id);
    void render_log_dialog();
    void open_in_viewer(const std::string& file);

    AppState&                            state_;
    GraphWindow*                         graph_window_    = nullptr;
    SettingsWindow*                      settings_window_ = nullptr;
    FlowEditorWindow*                    flow_window_     = nullptr;
    Window*                              flows_list_      = nullptr;
    std::vector<std::unique_ptr<Window>> windows_;
    std::atomic<bool>                    stop_requested_{false};
    std::string                          status_label_;
    std::function<bool()>                status_connected_fn_;
    ILogControl*                         log_controller_ = nullptr;

    // "Start logging" dialog
    bool        open_log_dialog_ = false;
    std::string log_path_;
    bool        remember_dir_    = false;
    std::string log_error_;
    FileBrowser log_browser_;
    // viewer launch error, shown in the menu bar for a few seconds
    std::string viewer_error_;
    double      viewer_error_time_ = 0.0;

    Mode                        mode_ = Mode::Live;
    std::unique_ptr<DebugView>  debug_view_;
    std::unique_ptr<DbcEditor>  dbc_editor_;

    bool  reset_layout_   = false;
    float status_width_   = 0.0f;  // width of the menu bar status group (last frame)
};
