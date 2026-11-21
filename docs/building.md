# Building caneo

caneo is built with **CMake** and **Conan 2**. All C++ dependencies come from
Conan, except Dear ImGui (docking branch) and ImPlot, which CMake downloads
itself (see [Dear ImGui](#dear-imgui-docking-branch)).

The recommended way is the Docker build. It needs nothing installed on the
host except Docker, and it works around two problems of native builds:

* **dbcppp does not compile with GCC ≥ 14.** Fedora and other current
  distributions ship GCC 15. The image uses Ubuntu 24.04 with GCC 13.
* **Cross builds for ARM** (Raspberry Pi, embedded Linux boards) use the
  same image under QEMU, without setting up a cross toolchain.

## TL;DR

```bash
./docker-build.sh                  # -> build/caneo         (x86_64, Release)
./docker-build.sh --arch arm64     # -> build-arm64/caneo   (aarch64, Release)
./docker-build.sh --debug          # -> build-debug/caneo
```

## Docker build in detail

### `docker-build.sh`

| Option            | Effect                                                         |
|-------------------|----------------------------------------------------------------|
| `--arch arm64`    | Build for aarch64 (via QEMU). Default: host architecture.      |
| `--debug`         | Debug build (`build-debug/`, `build-arm64-debug/`)             |
| `--clean`         | Delete the build directory first                               |
| `--shell`         | Interactive shell in the build container (repo at `/build`)   |
| `--rebuild-image` | Rebuild the Docker image (e.g. after editing the `Dockerfile`) |
| `-j N`            | Parallel compile jobs (default: `nproc`)                       |

What the script does:

1. It builds the image `caneo-builder:<arch>` from the [`Dockerfile`](../Dockerfile)
   if the image does not exist yet. The image has Ubuntu 24.04, GCC 13, CMake,
   Conan 2, and the X11/OpenGL dev packages that GLFW needs.
2. It mounts the repository at `/build` and the named volume
   `caneo-conan-<arch>` at `/root/.conan2`. The volume holds the **Conan
   package cache**, so boost, gRPC, protobuf and the other dependencies are
   compiled once per architecture. After that, a build only compiles caneo
   itself.
3. It runs `conan install`, `cmake` and `cmake --build`.
4. It `chown`s the build directory back to your user, so `build/` is no
   longer owned by root.

The first build of an architecture takes a while (gRPC alone takes
10–20 min on x86_64). Builds after that take about a minute.

> **SELinux (Fedora/RHEL):** the repository is mounted with `:Z`, which
> relabels it for container access. The script does this for you.

### ARM64 (cross build via QEMU)

Docker runs the arm64 image through QEMU user emulation. Register QEMU
once per boot, or permanently via your distribution's `qemu-user-static`
package:

```bash
docker run --privileged --rm tonistiigi/binfmt --install arm64
./docker-build.sh --arch arm64
```

Notes:

* Emulation is slow. The **first** arm64 build compiles all Conan dependencies
  under QEMU and can take **several hours** (gRPC is the main cost). After
  that, the `caneo-conan-arm64` volume has everything cached and builds take
  minutes.
* Cross builds link `libstdc++`/`libgcc` statically (`-DCANEO_STATIC_RUNTIME=ON`),
  so the target does not need GCC 13 runtime libraries. It still needs
  **glibc ≥ 2.39** (Ubuntu 24.04, Debian 13 "trixie", Raspberry Pi OS based on
  trixie). For older targets, build on an older base image, e.g.
  `docker build --build-arg BASE_IMAGE=... -t caneo-builder:arm64 --platform linux/arm64 .`
  However, it needs a C++23-capable GCC ≥ 13.
* Copy `build-arm64/caneo` to the target. Runtime requirements are the same
  as on x86_64: SocketCAN (kernel), and for `--gui` an X11/Wayland session
  with OpenGL 3.3.

### Using Docker manually

The image's default command still does a plain release build:

```bash
docker build -t caneo-builder:amd64 .
docker run --rm -v .:/build:Z -v caneo-conan-amd64:/root/.conan2 caneo-builder:amd64
```

Without the volume, every `docker run` starts with an empty Conan cache and
rebuilds all dependencies.

### Housekeeping

```bash
docker volume ls | grep caneo-conan       # cached dependencies per arch
docker volume rm caneo-conan-arm64        # forget them (next build recompiles)
docker image rm caneo-builder:amd64       # image is rebuilt on the next run
```

## Native build

This needs **GCC 13** (GCC ≥ 14 breaks dbcppp), CMake ≥ 3.25, and Conan 2:

```bash
pip install conan
conan profile detect

conan install . --output-folder=build --build=missing -c tools.system.package_manager:mode=install
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

`-c tools.system.package_manager:mode=install` lets Conan install the X11
development packages that `glfw` (via `xorg/system`) needs. It uses
sudo/apt/dnf.

For a debug build, pass `--settings=build_type=Debug` to conan and
`-DCMAKE_BUILD_TYPE=Debug` to CMake, ideally in a separate build directory.

## Dear ImGui (docking branch)

Conan's `imgui` package is built from the master branch, which has no
docking support. [`cmake/ImGui.cmake`](../cmake/ImGui.cmake) therefore fetches:

| Library   | Version (CMake cache variable)                       |
|-----------|-----------------------------------------------------|
| Dear ImGui| `CANEO_IMGUI_VERSION`  = `1.90.5-docking`            |
| ImPlot    | `CANEO_IMPLOT_VERSION` = `0.16`                      |

CMake downloads both as release tarballs from GitHub during configure
(SHA256-pinned) and builds them as static libraries, together with the
GLFW/OpenGL3 backends and `imgui_stdlib`. ImPlot must be compiled against the
same ImGui, which is why it is no longer taken from Conan.

The fonts Roboto (UI) and Cousine (script editor) from the ImGui repository
are embedded into the binary (`cmake/EmbedFile.cmake`). That way the GUI
scales without blurring and needs no font files at runtime.

**Offline builds:** point CMake at local checkouts:

```bash
cmake -B build ... \
  -DFETCHCONTENT_SOURCE_DIR_IMGUI=/path/to/imgui \
  -DFETCHCONTENT_SOURCE_DIR_IMPLOT=/path/to/implot
```

**Updating ImGui:** set a different `*-docking` tag and update `URL_HASH` in
`cmake/ImGui.cmake` (`curl -sL <url> | sha256sum`). Newer ImGui versions usually
also need a newer ImPlot commit.

## Installing as a service

See `install-service.sh` / `caneo.service` (headless logging with `--log`).
After each rebuild:

```bash
sudo cp build/caneo /usr/local/bin/caneo
sudo restorecon -v /usr/local/bin/caneo   # SELinux
sudo systemctl restart caneo
```

## Headless build (no GUI)

For targets without a display, the GUI (Dear ImGui, GLFW, OpenGL) can be left out:

```bash
conan install . --output-folder=build-nogui --build=missing -o "&:gui=False"
cmake -B build-nogui -DCMAKE_TOOLCHAIN_FILE=build-nogui/conan_toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release -DCANEO_GUI=OFF
```

Without a mode flag such a binary runs `--headless`; `--tui` and the gRPC
server/client work as usual. The Raspberry Pi 5 image (`plat/rpi5`, Yocto)
cross-builds caneo this way with the Yocto SDK, see `plat/rpi5/README.md`.
