# Dear ImGui (docking branch) + ImPlot, built as static libraries.
#
# The conan imgui package only ships the master branch (no docking), so both
# are fetched here. ImPlot has to be built against the very same ImGui, which
# is why it is fetched too instead of taken from conan.
#
# Offline builds: point FETCHCONTENT_SOURCE_DIR_IMGUI / _IMPLOT at local
# checkouts, e.g. -DFETCHCONTENT_SOURCE_DIR_IMGUI=$HOME/src/imgui
include(FetchContent)

set(CANEO_IMGUI_VERSION  "1.90.5-docking" CACHE STRING "Dear ImGui tag (must be a *-docking tag)")
set(CANEO_IMPLOT_VERSION "0.16"           CACHE STRING "ImPlot tag")
set(CANEO_IMNODES_VERSION "0.5"           CACHE STRING "imnodes tag (node editor)")

FetchContent_Declare(imgui
    URL      https://github.com/ocornut/imgui/archive/refs/tags/v${CANEO_IMGUI_VERSION}.tar.gz
    URL_HASH SHA256=8a5e1e594d6c8552e46e4c1ba8dd9deb51262067f04937904babc04384533ccc
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(implot
    URL      https://github.com/epezent/implot/archive/refs/tags/v${CANEO_IMPLOT_VERSION}.tar.gz
    URL_HASH SHA256=961df327d8a756304d1b0a67316eebdb1111d13d559f0d3415114ec0eb30abd1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(imnodes
    URL      https://github.com/Nelarius/imnodes/archive/refs/tags/v${CANEO_IMNODES_VERSION}.tar.gz
    URL_HASH SHA256=c19a1d8e3fabf71def02b98c43a3f4551f0a5bd3740a93474a356e8957ec2ab2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# None of these projects has a CMakeLists.txt that we use, so MakeAvailable only downloads them.
FetchContent_MakeAvailable(imgui implot imnodes)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

add_library(implot STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot_SOURCE_DIR}/implot_demo.cpp)
target_include_directories(implot SYSTEM PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot PUBLIC imgui)

# imnodes: node editor for the flow editor, built against the same ImGui.
add_library(imnodes STATIC ${imnodes_SOURCE_DIR}/imnodes.cpp)
target_include_directories(imnodes SYSTEM PUBLIC ${imnodes_SOURCE_DIR})
target_compile_definitions(imnodes PRIVATE IMGUI_DEFINE_MATH_OPERATORS)   # imnodes uses ImVec2 operators
target_link_libraries(imnodes PUBLIC imgui)

# Fonts shipped with ImGui, embedded into the binary so that the GUI can be
# scaled with crisp text (the built-in ProggyClean bitmap font blurs).
include(${CMAKE_CURRENT_LIST_DIR}/EmbedFile.cmake)
set(CANEO_FONTS_CPP "${CMAKE_BINARY_DIR}/generated/embedded_fonts.cpp")
embed_files("${CANEO_FONTS_CPP}"
    caneo_font_ui   "${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf"
    caneo_font_mono "${imgui_SOURCE_DIR}/misc/fonts/Cousine-Regular.ttf")
