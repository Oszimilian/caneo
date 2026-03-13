cmake_minimum_required(VERSION 3.25)
project(caneo CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED COMPONENTS system)
find_package(dbcppp REQUIRED)
find_package(ftxui REQUIRED)
find_package(yaml-cpp REQUIRED)

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
)

target_include_directories(caneo PRIVATE src)
target_link_libraries(caneo PRIVATE Boost::system dbcppp::dbcppp ftxui::component ftxui::dom ftxui::screen yaml-cpp::yaml-cpp)
