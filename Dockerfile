# Build environment for caneo (see docs/building.md).
#
# Ubuntu 24.04 ships GCC 13, which is the newest GCC that still compiles
# dbcppp. The same Dockerfile works for amd64 and arm64 (via --platform).
ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain + the X11/OpenGL dev packages that conan's xorg/system and
# opengl/system recipes would otherwise apt-install on every build.
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc-13 g++-13 \
    cmake make \
    python3 python3-pip \
    pkg-config ca-certificates \
    libgl-dev \
    libx11-dev libx11-xcb-dev libfontenc-dev libice-dev libsm-dev libxau-dev \
    libxaw7-dev libxcomposite-dev libxcursor-dev libxdamage-dev libxdmcp-dev \
    libxext-dev libxfixes-dev libxi-dev libxinerama-dev libxkbfile-dev \
    libxmu-dev libxmuu-dev libxpm-dev libxrandr-dev libxrender-dev libxres-dev \
    libxss-dev libxt-dev libxtst-dev libxv-dev libxxf86vm-dev libxcb-glx0-dev \
    libxcb-render0-dev libxcb-render-util0-dev libxcb-xkb-dev libxcb-icccm4-dev \
    libxcb-image0-dev libxcb-keysyms1-dev libxcb-randr0-dev libxcb-shape0-dev \
    libxcb-sync-dev libxcb-xfixes0-dev libxcb-xinerama0-dev libxcb-dri3-dev \
    libxcb-cursor-dev libxcb-dri2-0-dev libxcb-present-dev \
    libxcb-composite0-dev libxcb-ewmh-dev libxcb-res0-dev libxcb-util-dev \
    uuid-dev \
    && rm -rf /var/lib/apt/lists/*

RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
 && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100 \
 && update-alternatives --install /usr/bin/cc  cc  /usr/bin/gcc-13 100 \
 && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-13 100

RUN pip3 install conan --break-system-packages

# The default profile ends up in /root/.conan2. docker-build.sh mounts a
# named volume there; Docker seeds an empty volume with this content.
RUN conan profile detect --force

WORKDIR /build

# Plain `docker run -v .:/build:Z caneo-builder` still works; docker-build.sh
# is the recommended entry point (persistent conan cache, arm64, file owner).
CMD ["bash", "-c", \
    "conan install . --output-folder=build --build=missing -c tools.system.package_manager:mode=install && \
     cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release && \
     cmake --build build -j$(nproc)"]
