#pragma once

#include "AppState.hpp"
#include "FileBrowser.hpp"
#include "ImNodesPan.hpp"
#include "Window.hpp"
#include "setup/CanDevices.hpp"
#include "grpc/RemoteInterfaces.hpp"

#include <expected>
#include <future>
#include <map>
#include <optional>

#include <format>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

struct ImNodesContext;
struct ImNodesEditorContext;

// Settings > Channels: graphical channel setup (config `pipeline:`). Devices (hardware / virtual SocketCAN) -> baudrate ->
// decode (DBC) -> live view / log. Edits Config::pipeline; Config::interfaces is derived from it.
// Channel changes need a restart.
class PipelineWindow : public Window {
public:
    explicit PipelineWindow(AppState& state);
    ~PipelineWindow() override;

    const char* title() const override { return "Inputs"; }
    void render() override;
    void render_contents();   // toolbar + canvas, embedded in the Settings tab

    friend std::ostream& operator<<(std::ostream& os, const PipelineWindow& w);

private:
    void render_node(PipelineNodeSpec& n);
    void render_add_popup();
    void render_block_menu();
    void add_node(const std::string& type, float x, float y);
    void remove_node(int id);
    void set_channels(PipelineNodeSpec& n, std::vector<std::string> channels);
    PipelineNodeSpec* find(int id);
    void refresh_devices();
    void render_grpc_node(PipelineNodeSpec& n);
    void query_remote(const PipelineNodeSpec& n);

    // gRPC SocketCAN: ListInterfaces of the server, per block (worker thread, polled while rendering)
    using RemoteResult = std::expected<RemoteInterfaceList, std::string>;
    struct RemoteQuery {
        std::string                    address;   // the query was made for this address
        std::future<RemoteResult>      pending;
        std::optional<RemoteResult>    result;
    };
    std::map<int, RemoteQuery> remote_;

    AppState&             state_;
    ImNodesContext*       ctx_ = nullptr;
    ImNodesEditorContext* editor_ = nullptr;
    std::string           theme_;
    ImNodesZoom           zoom_;

    PipelineSpec last_;                 // to notice edits
    bool         synced_ = false;
    std::string  last_path_;
    bool         positions_dirty_ = true;
    int          next_id_ = 1;

    bool  open_add_popup_ = false;
    float add_x_ = 0.0f, add_y_ = 0.0f;

    std::vector<CanDevice> devices_;
    double                 devices_time_ = -1.0;

    FileBrowser browser_;
    int         browse_node_ = 0;
    bool        open_browser_ = false;
};

template <>
struct std::formatter<PipelineWindow> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const PipelineWindow& obj, std::format_context& ctx) const {
        std::ostringstream ss;
        ss << obj;
        return std::format_to(ctx.out(), "{}", ss.str());
    }
};
