cmake_minimum_required(VERSION 3.25)
project(caneo CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED COMPONENTS system)
find_package(dbcppp REQUIRED)
find_package(ftxui REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(protobuf REQUIRED)
find_package(mcap REQUIRED)
find_package(lua REQUIRED)
find_package(sol2 REQUIRED)
find_package(gflags REQUIRED)
find_package(gRPC REQUIRED)
find_package(ZLIB REQUIRED)   # BLF containers; comes with grpc/protobuf
find_package(elfio REQUIRED)     # XCP: ELF symbols
find_package(libdwarf REQUIRED)  # XCP: DWARF types

option(CANEO_STATIC_RUNTIME "Link libstdc++/libgcc statically (portable binaries)" OFF)
# OFF = headless build (TUI / --headless / gRPC only) without GLFW/OpenGL/Dear ImGui,
# e.g. for the Raspberry Pi image (plat/rpi5)
option(CANEO_GUI "Build the Dear ImGui GUI (needs GLFW + OpenGL)" ON)

if(CANEO_GUI)
    find_package(glfw3 REQUIRED)
    find_package(OpenGL REQUIRED)
    include(cmake/ImGui.cmake)
endif()

add_executable(caneo
    src/main.cpp
    src/frame/DataFrame.cpp
    src/frame/CanFrame.cpp
    src/frame/DataFrameSet.cpp
    src/socket/Socket.cpp
    src/socket/SocketCAN.cpp
    src/socket/SocketGrpc.cpp
    src/decoder/DbcPppWrapper.cpp
    src/decoder/DecoderRegistry.cpp
    src/gui/TuiDataFrameSet.cpp
    src/gui/LogViewer.cpp
    src/send/ModelCache.cpp
    src/dbc/DbcModel.cpp
    src/debug/LogDocument.cpp
    src/debug/DbcInfo.cpp
    src/debug/OfflineFlow.cpp
    src/debug/CsvExport.cpp
    src/config/Config.cpp
    src/config/Pipeline.cpp
    src/config/RecentConfigs.cpp
    src/setup/CanDevices.cpp
    src/config/AppConfig.cpp
    src/setup/InterfaceSetup.cpp
    src/send/SendModel.cpp
    src/action/Action.cpp
    src/action/ActionHandler.cpp
    src/action/ActionFactory.cpp
    src/action/LuaAction.cpp
    src/guard/Guard.cpp
    src/guard/GuardEngine.cpp
    src/flow/Flow.cpp
    src/flow/Legacy.cpp
    src/flow/FlowEngine.cpp
    src/grpc/CanStreamService.cpp
    src/grpc/CanStreamServer.cpp
    src/grpc/RemoteLogControl.cpp
    src/grpc/RemoteInterfaces.cpp
    src/proto/ProtoLog.cpp
    src/proto/ProtoLogRegistry.cpp
    src/logger/LogController.cpp
    src/logger/McapLogger.cpp
    src/logger/McapReader.cpp
    src/logger/BlfWriter.cpp
    src/logger/BlfReader.cpp
    src/model/SignalStore.cpp
    src/model/ModelEngine.cpp
    src/modes/run_playback.cpp
    src/modes/run_tui.cpp
    src/modes/config_picker.cpp
    src/modes/run_live.cpp
    src/xcp/A2lReader.cpp
    src/xcp/ElfReader.cpp
    src/xcp/XcpCanTransport.cpp
    src/xcp/XcpMaster.cpp
)

if(CANEO_GUI)
    target_sources(caneo PRIVATE
        src/gui/Gui.cpp
        src/gui/AppState.cpp
        src/gui/GraphPreset.cpp
        src/gui/GraphWindow.cpp
        src/gui/ModelWindow.cpp
        src/gui/TraceWindow.cpp
        src/gui/TraceContainerWindow.cpp
        src/gui/SendWindow.cpp
        src/gui/FileBrowser.cpp
        src/gui/SettingsWindow.cpp
        src/gui/GuiStyle.cpp
        src/gui/FlowsWindow.cpp
        src/gui/FlowEditorWindow.cpp
        src/gui/DebugView.cpp
        src/gui/DbcEditor.cpp
        src/gui/DbcNetwork.cpp
        src/gui/PipelineWindow.cpp
        src/gui/XcpView.cpp
        src/modes/run_gui.cpp
        ${CANEO_FONTS_CPP}
    )
    target_link_libraries(caneo PRIVATE imgui implot imnodes)
    target_compile_definitions(caneo PRIVATE CANEO_HAS_GUI=1)
else()
    target_compile_definitions(caneo PRIVATE CANEO_HAS_GUI=0)
endif()

# gRPC / protobuf codegen for can_stream.proto
set(PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/proto/can_stream.proto")
set(PROTO_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY "${PROTO_OUT_DIR}")

if(CMAKE_CROSSCOMPILING)
    # The conan targets point at the target-arch binaries; the native tools of the
    # build context are on PATH (conanbuild.sh), e.g. for plat/rpi5.
    find_program(PROTOC_EXE protoc REQUIRED)
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
else()
    get_target_property(PROTOC_EXE protobuf::protoc LOCATION)
    get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin LOCATION)
endif()

add_custom_command(
    OUTPUT
        "${PROTO_OUT_DIR}/can_stream.pb.cc"
        "${PROTO_OUT_DIR}/can_stream.pb.h"
        "${PROTO_OUT_DIR}/can_stream.grpc.pb.cc"
        "${PROTO_OUT_DIR}/can_stream.grpc.pb.h"
    COMMAND "${PROTOC_EXE}"
    ARGS
        "--cpp_out=${PROTO_OUT_DIR}"
        "--grpc_out=${PROTO_OUT_DIR}"
        "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}"
        "-I${CMAKE_CURRENT_SOURCE_DIR}/src/proto"
        "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Generating gRPC sources from can_stream.proto"
)

target_sources(caneo PRIVATE
    "${PROTO_OUT_DIR}/can_stream.pb.cc"
    "${PROTO_OUT_DIR}/can_stream.grpc.pb.cc"
)
target_include_directories(caneo PRIVATE "${PROTO_OUT_DIR}")

target_include_directories(caneo PRIVATE src)
target_link_libraries(caneo PRIVATE
    Boost::system
    dbcppp::dbcppp
    ftxui::component ftxui::dom ftxui::screen
    yaml-cpp::yaml-cpp
    protobuf::libprotobuf
    mcap::mcap
    lua::lua
    sol2::sol2
    gflags::gflags
    gRPC::grpc++
    ZLIB::ZLIB
    elfio::elfio
    libdwarf::libdwarf
)

if(CANEO_STATIC_RUNTIME)
    target_link_options(caneo PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Small self-checks without extra test framework: `ctest --test-dir build`
option(CANEO_BUILD_TESTS "Build caneo_config_test" ON)
if(CANEO_BUILD_TESTS)
    enable_testing()
    add_executable(caneo_config_test tests/config_roundtrip.cpp src/config/Config.cpp src/config/Pipeline.cpp)
    target_include_directories(caneo_config_test PRIVATE src)
    target_link_libraries(caneo_config_test PRIVATE yaml-cpp::yaml-cpp)
    add_test(NAME config_roundtrip
             COMMAND caneo_config_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/full.yaml
                                       ${CMAKE_CURRENT_BINARY_DIR}/roundtrip.yaml
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)

    add_test(NAME config_roundtrip_xcp
             COMMAND caneo_config_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/xcp.yaml
                                       ${CMAKE_CURRENT_BINARY_DIR}/roundtrip_xcp.yaml
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)

    # XCP: ELF reader self-check + a command line master for manual tests against a slave (scripts/xcp_sim.py)
    add_executable(caneo_xcp_test tests/xcp_test.cpp src/xcp/A2lReader.cpp src/xcp/ElfReader.cpp src/xcp/XcpCanTransport.cpp src/xcp/XcpMaster.cpp)
    target_include_directories(caneo_xcp_test PRIVATE src)
    target_link_libraries(caneo_xcp_test PRIVATE elfio::elfio libdwarf::libdwarf)
    add_test(NAME xcp_codec COMMAND caneo_xcp_test --selftest)
endif()
