#!/usr/bin/env bash
#
# Yocto (scarthgap) image for the Raspberry Pi 5 + PEAK PCAN-M.2 with caneo
# as headless gRPC server. Everything runs in the Docker image "caneo-yocto".
#
#   ./build.sh            # all steps: sdk -> caneo -> image
#   ./build.sh sdk        # bitbake meta-toolchain + install the SDK to sdk/install
#   ./build.sh caneo      # cross-build caneo with the SDK (+ conan) -> meta-caneo/.../files/caneo
#   ./build.sh image      # bitbake caneo-image -> deploy/caneo-image-raspberrypi5.rootfs.wic.bz2
#   ./build.sh shell      # shell in the build container (bitbake environment)
#
# Layout (plat/rpi5):
#   layers/      poky, meta-raspberrypi, meta-openembedded (scarthgap, see layers.txt)
#   meta-caneo/  own layer: kernel CAN config, caneo + network recipes, caneo-image
#   conf/        local.conf / bblayers.conf templates (copied into build/conf)
#   build/       bitbake build dir, cache/ = downloads + sstate, sdk/ = installed SDK + conan cache
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
IMAGE=caneo-yocto
STEP="${1:-all}"
[[ -t 0 && -t 1 ]] && TTY=(-it) || TTY=()

clone_layers() {
    mkdir -p "$HERE/layers"
    while read -r name url rev; do
        [[ -z "$name" || "$name" == \#* ]] && continue
        if [[ ! -d "$HERE/layers/$name" ]]; then
            git clone -q "$url" "$HERE/layers/$name"
            git -C "$HERE/layers/$name" checkout -q "$rev"
        fi
    done < "$HERE/layers.txt"
}

docker_image() {
    if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
        docker build -t "$IMAGE" --build-arg UID="$(id -u)" --build-arg GID="$(id -g)" "$HERE/docker"
    fi
}

# run <bash script> inside the container; repo at /repo, plat/rpi5 at /work
run() {
    docker run --rm "${TTY[@]}" \
        -v "$REPO":/repo:Z -w /repo/plat/rpi5 \
        -e HOME=/repo/plat/rpi5/sdk/home \
        "$IMAGE" bash -c "$1"
}

BB_ENV='set -e
mkdir -p build/conf
cp -n conf/local.conf conf/bblayers.conf build/conf/ 2>/dev/null || true
source layers/poky/oe-init-build-env build >/dev/null'

step_sdk() {
    run "$BB_ENV
bitbake meta-toolchain
sh=\$(ls -t tmp/deploy/sdk/poky-glibc-x86_64-meta-toolchain-*.sh | head -1)
rm -rf ../sdk/install
\$sh -y -d /repo/plat/rpi5/sdk/install >/dev/null
echo \">>> SDK installed: sdk/install\""
}

step_caneo() {
    run 'set -e
mkdir -p sdk/home
source sdk/install/environment-setup-cortexa76-poky-linux
# conan profile for the Pi from the SDK environment: compiler + target flags + sysroot.
# The build context (protoc, grpc_cpp_plugin, b2) uses the container gcc.
cc=(${CC}); cxx=(${CXX})
cflags="${cc[*]:1} ${CFLAGS}"; cxxflags="${cxx[*]:1} ${CXXFLAGS}"
ldflags="${cc[*]:1} ${LDFLAGS}"
toolbin=$(dirname "$(command -v ${cc[0]})")
mkdir -p "$HOME/.conan2/profiles"
pylist() { python3 -c "import json,shlex,sys; print(json.dumps(shlex.split(sys.argv[1])))" "$1"; }
cat > "$HOME/.conan2/profiles/rpi5" <<EOF
[settings]
os=Linux
arch=armv8
compiler=gcc
compiler.version=13
compiler.libcxx=libstdc++11
compiler.cppstd=gnu17
build_type=Release
[conf]
tools.build:compiler_executables={"c": "$toolbin/${cc[0]}", "cpp": "$toolbin/${cxx[0]}"}
tools.build:sysroot=$SDKTARGETSYSROOT
tools.build:cflags=$(pylist "$cflags")
tools.build:cxxflags=$(pylist "$cxxflags")
tools.build:sharedlinkflags=$(pylist "$ldflags")
tools.build:exelinkflags=$(pylist "$ldflags")
tools.cmake.cmaketoolchain:system_processor=aarch64
tools.gnu:host_triplet=aarch64-poky-linux
[buildenv]
CC=$toolbin/${cc[0]} ${cc[*]:1}
CXX=$toolbin/${cxx[0]} ${cxx[*]:1}
AR=$toolbin/aarch64-poky-linux-ar
RANLIB=$toolbin/aarch64-poky-linux-ranlib
STRIP=$toolbin/aarch64-poky-linux-strip
NM=$toolbin/aarch64-poky-linux-nm
LD=$toolbin/aarch64-poky-linux-ld
EOF
# drop the SDK variables again: the build context must compile natively
unset CC CXX CPP AR AS LD NM RANLIB STRIP OBJCOPY OBJDUMP CFLAGS CXXFLAGS CPPFLAGS LDFLAGS \
      PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_PATH CONFIG_SITE OECORE_NATIVE_SYSROOT CMAKE_TOOLCHAIN_FILE \
      OE_CMAKE_TOOLCHAIN_FILE OE_CMAKE_FIND_LIBRARY_CUSTOM_LIB_SUFFIX KCFLAGS M4 TARGET_PREFIX CROSS_COMPILE ARCH CONFIGURE_FLAGS
export PATH=/usr/local/bin:/usr/bin:/bin
# build profile = native container gcc (detected without the SDK variables)
conan profile detect --force >/dev/null 2>&1
cd /repo
B=plat/rpi5/sdk/build-caneo
conan install . --output-folder=$B --build=missing -pr:h=rpi5 -pr:b=default -o "&:gui=False"
source $B/conanbuild.sh
cmake -B $B -DCMAKE_TOOLCHAIN_FILE=$B/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
      -DCANEO_GUI=OFF -DCANEO_BUILD_TESTS=OFF
cmake --build $B -j$(nproc)
cp plat/rpi5/sdk/build-caneo/caneo plat/rpi5/meta-caneo/recipes-apps/caneo/files/caneo
plat/rpi5/sdk/install/sysroots/x86_64-pokysdk-linux/usr/bin/aarch64-poky-linux/aarch64-poky-linux-strip \
    plat/rpi5/meta-caneo/recipes-apps/caneo/files/caneo
file plat/rpi5/meta-caneo/recipes-apps/caneo/files/caneo
echo ">>> caneo for the Pi: plat/rpi5/meta-caneo/recipes-apps/caneo/files/caneo"'
}

step_image() {
    [[ -f "$HERE/meta-caneo/recipes-apps/caneo/files/caneo" ]] || { echo "run ./build.sh caneo first" >&2; exit 1; }
    run "$BB_ENV
bitbake caneo-image
mkdir -p ../deploy
cp -L tmp/deploy/images/raspberrypi5/caneo-image-raspberrypi5.rootfs.wic.bz2 \
      tmp/deploy/images/raspberrypi5/caneo-image-raspberrypi5.rootfs.wic.bmap ../deploy/
ls -la ../deploy"
}

clone_layers
docker_image
case "$STEP" in
    sdk)   step_sdk ;;
    caneo) step_caneo ;;
    image) step_image ;;
    all)   step_sdk; step_caneo; step_image ;;
    shell) docker run --rm -it -v "$REPO":/repo:Z -w /repo/plat/rpi5 -e HOME=/repo/plat/rpi5/sdk/home "$IMAGE" \
               bash -c "$BB_ENV; exec bash" ;;
    *) sed -n '3,18p' "$0"; exit 1 ;;
esac
