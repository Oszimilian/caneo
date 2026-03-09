from conan import ConanFile


class CaneoConan(ConanFile):
    name = "caneo"
    version = "0.1.0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeToolchain", "CMakeDeps"

    def requirements(self):
        self.requires("boost/1.86.0")
