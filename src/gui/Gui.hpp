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
#include <thread>
#include <vector>

class DebugView;
class FlowEditorWindow;
class ModelWindow;
class PipelineWindow;
class DbcEditor;
class XcpView;
class ILogControl;
class SettingsWindow;

class Gui {
public:
    // iface_configs: the running channels (one trace tab each).
    // Live = the normal windows; Debug = full-screen offline log viewer.
    enum class Mode { Live, Debug, Dbc, Xcp, Settings };

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

    // "Start" / "Stop" in the menu bar: start sets the interfaces up (may ask for the password) and opens the sockets,
    // stop closes them. Nothing is opened before the user presses Start.
    struct ChannelResult {
        bool        running = false;   // at least one channel is open
        std::string message;
    };
    void          set_channel_control(std::function<ChannelResult(bool graphical)> start, std::function<void()> stop);
    ChannelResult start_channels_now(bool graphical);   // synchronous (used for --log)

    // "Reset HW" next to Stop: takes the running channels down + up (local, and remote ones via gRPC).
    // Runs on a worker thread (may ask for the password); returns an error text, empty on success.
    void set_reset_control(std::function<std::string(bool graphical)> reset) { channel_reset_ = std::move(reset); }

    // No config file was found: show the Settings with the "load config" dialog open.
    void search_config() { search_config_ = true; }

private:
    void render_menu_bar();
    void render_mode_toggle(); // centre of the menu bar
    void render_status(); // right side of the menu bar
    void render_channel_button();
    void render_reset_button();
    void render_welcome();
    void build_default_layout(unsigned int dockspace_id);
    void render_log_dialog();
    void open_in_viewer(const std::string& file);

    AppState&                            state_;
    GraphWindow*                         graph_window_    = nullptr;
    SettingsWindow*                      settings_window_ = nullptr;
    FlowEditorWindow*                    flow_window_     = nullptr;
    Window*                              flows_list_      = nullptr;
    std::unique_ptr<PipelineWindow>      pipeline_;   // lives in the Settings tab
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

    // channel start / stop
    std::function<ChannelResult(bool)> channel_start_;
    std::function<void()>              channel_stop_;
    std::atomic<bool>                  channels_running_{false};
    std::atomic<bool>                  channels_starting_{false};
    ChannelResult                      start_result_;
    std::jthread                       starter_;
    std::function<std::string(bool)>   channel_reset_;
    std::atomic<bool>                  channels_resetting_{false};
    std::string                        reset_result_;
    std::jthread                       resetter_;
    bool                               search_config_ = false;
    std::vector<std::string>           recent_;
    FileBrowser                        welcome_browser_;

    Mode                        mode_ = Mode::Live;
    std::unique_ptr<DebugView>  debug_view_;
    std::unique_ptr<DbcEditor>  dbc_editor_;
    std::unique_ptr<XcpView>    xcp_view_;

    bool  reset_layout_   = false;
    float status_width_   = 0.0f;  // width of the menu bar status group (last frame)
};
