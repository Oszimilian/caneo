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
    src/config/Config.cpp
    src/setup/InterfaceSetup.cpp
    src/send/SendModel.cpp
    src/action/Action.cpp
    src/action/ActionHandler.cpp
    src/proto/ProtoLog.cpp
    src/proto/ProtoLogRegistry.cpp
    src/logger/McapLogger.cpp
    src/logger/McapReader.cpp
    src/model/SignalStore.cpp
    src/model/ModelEngine.cpp
)

target_include_directories(caneo PRIVATE src)
target_link_libraries(caneo PRIVATE Boost::system dbcppp::dbcppp ftxui::component ftxui::dom ftxui::screen yaml-cpp::yaml-cpp protobuf::libprotobuf mcap::mcap lua::lua sol2::sol2)
