from conan import ConanFile
from conan.tools.build import cross_building


class CaneoConan(ConanFile):
    name = "caneo"
    version = "0.1.0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeToolchain", "CMakeDeps"
    # gui=False: headless build without GLFW/OpenGL (pass -DCANEO_GUI=OFF to CMake too)
    options = {"gui": [True, False]}
    default_options = {"gui": True}

    def requirements(self):
        self.requires("boost/1.80.0")
        self.requires("dbcppp/3.2.6")
        self.requires("ftxui/6.1.9")
        self.requires("yaml-cpp/0.8.0")
        self.requires("protobuf/3.21.12")
        self.requires("grpc/1.54.3")
        self.requires("mcap/2.1.1")
        self.requires("lua/5.4.6")
        self.requires("sol2/3.3.0")
        self.requires("gflags/2.2.2")
        if self.options.gui:
            self.requires("glfw/3.4")
        self.requires("elfio/3.12")      # ELF sections / symbols (XCP)
        self.requires("libdwarf/2.1.0")  # DWARF types of the ELF variables (XCP)
        # Dear ImGui (docking branch) and ImPlot are fetched by CMake, see
        # cmake/ImGui.cmake: conan only ships the master branch without docking.

    def build_requirements(self):
        # Cross builds (plat/rpi5): native protoc + grpc_cpp_plugin on PATH (conanbuild.sh)
        if cross_building(self):
            self.tool_requires("protobuf/3.21.12")
            self.tool_requires("grpc/1.54.3")
