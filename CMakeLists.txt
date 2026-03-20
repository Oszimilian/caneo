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
find_package(imgui REQUIRED)
find_package(implot REQUIRED)
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)

add_executable(caneo
    src/main.cpp
    src/frame/DataFrame.cpp
    src/frame/CanFrame.cpp
    src/frame/DataFrameSet.cpp
    src/socket/Socket.cpp
    src/socket/SocketCAN.cpp
    src/decoder/DbcPppWrapper.cpp
    src/decoder/DecoderRegistry.cpp
    src/gui/TuiDataFrameSet.cpp
    src/gui/Gui.cpp
    src/gui/GraphWindow.cpp
    src/gui/TraceWindow.cpp
    src/gui/SendWindow.cpp
    src/gui/ActionsWindow.cpp
    src/config/Config.cpp
    src/config/AppConfig.cpp
    src/setup/InterfaceSetup.cpp
    src/send/SendModel.cpp
    src/action/Action.cpp
    src/action/ActionHandler.cpp
    src/action/ActionPreset.cpp
    src/proto/ProtoLog.cpp
    src/proto/ProtoLogRegistry.cpp
    src/logger/McapLogger.cpp
    src/logger/McapReader.cpp
    src/model/SignalStore.cpp
    src/model/ModelEngine.cpp
    src/modes/run_playback.cpp
    src/modes/run_tui.cpp
    src/modes/run_live.cpp
    src/modes/run_gui.cpp
)

# ImGui GLFW + OpenGL3 backends are not shipped by the Conan package.
# Download them from GitHub at configure time (matching the installed version).
set(IMGUI_BACKENDS_DIR "${CMAKE_BINARY_DIR}/imgui_backends")
set(IMGUI_BACKENDS_URL "https://raw.githubusercontent.com/ocornut/imgui/v1.90.5/backends")
file(MAKE_DIRECTORY "${IMGUI_BACKENDS_DIR}")
foreach(f imgui_impl_glfw.h imgui_impl_glfw.cpp
          imgui_impl_opengl3.h imgui_impl_opengl3.cpp
          imgui_impl_opengl3_loader.h)
    if(NOT EXISTS "${IMGUI_BACKENDS_DIR}/${f}")
        message(STATUS "Downloading imgui backend: ${f}")
        file(DOWNLOAD "${IMGUI_BACKENDS_URL}/${f}" "${IMGUI_BACKENDS_DIR}/${f}" STATUS dl_status)
        list(GET dl_status 0 dl_err)
        if(dl_err)
            message(FATAL_ERROR "Failed to download imgui backend ${f}: ${dl_status}")
        endif()
    endif()
endforeach()

target_sources(caneo PRIVATE
    "${IMGUI_BACKENDS_DIR}/imgui_impl_glfw.cpp"
    "${IMGUI_BACKENDS_DIR}/imgui_impl_opengl3.cpp"
)
target_include_directories(caneo PRIVATE "${IMGUI_BACKENDS_DIR}")

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
    imgui::imgui
    implot::implot
    glfw
    OpenGL::GL
)
