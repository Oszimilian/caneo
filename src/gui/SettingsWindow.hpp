#pragma once

#include "AppState.hpp"
#include "FileBrowser.hpp"
#include "Window.hpp"

class PipelineWindow;

#include <optional>
#include <string>
#include <vector>

// Settings: GUI scale/theme, config file load/save and CAN channel setup.
class SettingsWindow : public Window {
public:
    explicit SettingsWindow(AppState& state);

    const char* title() const override { return "Settings"; }
    void render() override;
    void render_full();   // Settings mode: the whole work area

    // Opens the load / save-as dialog (also used by the main menu).
    void open_load_dialog();
    void open_save_as_dialog();
    void set_pipeline(PipelineWindow* p) { pipeline_ = p; }

    // Renders the dialogs; call every frame, also while the window is hidden.
    void render_dialogs();

private:
    struct DetectedIface {
        std::string name;
        std::string state;   // operstate
        bool        is_virtual;
    };

    void render_body();
    void render_appearance();
    void render_config_file();
    void render_logging();
    void render_remote();
    void refresh_detected();

    AppState&                  state_;
    PipelineWindow*            pipeline_ = nullptr;   // channel graph shown in the "Channels" tab
    float                      scale_edit_ = 1.0f;
    bool                       scale_dragging_ = false;
    std::vector<DetectedIface> detected_;
    double                     detected_time_ = -1.0;

    enum class Dialog { None, Dbc, Load, SaveAs, ViewerExe, OpenLog };
    FileBrowser browser_;
    Dialog      dialog_     = Dialog::None;
    int         dialog_row_ = -1;

    std::optional<std::string> detected_viewer_;   // PlotJuggler found on disk
    bool                       viewer_detected_ = false;
    std::string                viewer_error_;
};
