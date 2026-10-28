#!/usr/bin/env bash
#
# Builds caneo inside the caneo-builder Docker image (see docs/building.md).
#
#   ./docker-build.sh                 # Release build for this machine (amd64) -> build/caneo
#   ./docker-build.sh --arch arm64    # arm64 build via QEMU             -> build-arm64/caneo
#   ./docker-build.sh --debug         # Debug build                      -> build-debug/caneo
#   ./docker-build.sh --clean         # wipe the build directory first
#   ./docker-build.sh --shell         # interactive shell in the build container
#   ./docker-build.sh --rebuild-image # force a fresh `docker build` of the image
#
# The conan package cache lives in the named volume caneo-conan-<arch>, so
# dependencies (boost, grpc, ...) are only compiled once per architecture.
set -euo pipefail

ARCH="$(uname -m)"
case "$ARCH" in
    x86_64)  ARCH=amd64 ;;
    aarch64) ARCH=arm64 ;;
esac
HOST_ARCH="$ARCH"
BUILD_TYPE=Release
CLEAN=0
SHELL_MODE=0
REBUILD_IMAGE=0
JOBS="$(nproc)"

usage() {
    sed -n '3,14p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)          ARCH="$2"; shift 2 ;;
        --arch=*)        ARCH="${1#*=}"; shift ;;
        --debug)         BUILD_TYPE=Debug; shift ;;
        --clean)         CLEAN=1; shift ;;
        --shell)         SHELL_MODE=1; shift ;;
        --rebuild-image) REBUILD_IMAGE=1; shift ;;
        -j)              JOBS="$2"; shift 2 ;;
        -j*)             JOBS="${1#-j}"; shift ;;
        -h|--help)       usage 0 ;;
        *) echo "Unknown option: $1" >&2; usage 1 ;;
    esac
done

case "$ARCH" in
    amd64|arm64) ;;
    *) echo "Unsupported arch '$ARCH' (use amd64 or arm64)" >&2; exit 1 ;;
esac

cd "$(dirname "$(readlink -f "$0")")"

PLATFORM="linux/${ARCH}"
IMAGE="caneo-builder:${ARCH}"
VOLUME="caneo-conan-${ARCH}"

# Build directory: build/ for the native release build (keeps the existing
# service/install scripts working), otherwise build-<arch>[-debug].
BUILD_DIR=build
[[ "$ARCH" != "$HOST_ARCH" ]] && BUILD_DIR="build-${ARCH}"
[[ "$BUILD_TYPE" == Debug ]] && BUILD_DIR="${BUILD_DIR}-debug"

if [[ "$ARCH" != "$HOST_ARCH" ]] && [[ ! -e "/proc/sys/fs/binfmt_misc/qemu-aarch64" ]] && [[ "$ARCH" == arm64 ]]; then
    cat >&2 <<'EOF'
QEMU user emulation for arm64 is not registered. Run once (as a user that may use docker):

    docker run --privileged --rm tonistiigi/binfmt --install arm64

EOF
    exit 1
fi

if [[ "$REBUILD_IMAGE" == 1 ]] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo ">>> Building image $IMAGE"
    docker build --platform "$PLATFORM" -t "$IMAGE" .
fi

docker volume inspect "$VOLUME" >/dev/null 2>&1 || docker volume create "$VOLUME" >/dev/null

if [[ "$CLEAN" == 1 && -d "$BUILD_DIR" ]]; then
    echo ">>> Removing $BUILD_DIR"
    docker run --rm --platform "$PLATFORM" -v "$PWD":/build:Z "$IMAGE" rm -rf "/build/$BUILD_DIR"
fi

# Static libstdc++/libgcc for cross builds: the target only needs a
# compatible glibc, not GCC 13's libstdc++.
STATIC_RUNTIME=OFF
[[ "$ARCH" != "$HOST_ARCH" ]] && STATIC_RUNTIME=ON

DOCKER_ARGS=(--rm --platform "$PLATFORM"
             -v "$PWD":/build:Z
             -v "$VOLUME":/root/.conan2
             -w /build)

if [[ "$SHELL_MODE" == 1 ]]; then
    exec docker run -it "${DOCKER_ARGS[@]}" "$IMAGE" bash
fi

HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

echo ">>> Building caneo ($ARCH, $BUILD_TYPE) in $BUILD_DIR"
docker run "${DOCKER_ARGS[@]}" "$IMAGE" bash -c "
    set -e
    trap 'chown -R ${HOST_UID}:${HOST_GID} /build/${BUILD_DIR} 2>/dev/null || true' EXIT
    conan install . --output-folder=${BUILD_DIR} --build=missing \
        -s build_type=${BUILD_TYPE} \
        -c tools.system.package_manager:mode=install
    cmake -B ${BUILD_DIR} \
        -DCMAKE_TOOLCHAIN_FILE=${BUILD_DIR}/conan_toolchain.cmake \
        -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
        -DCANEO_STATIC_RUNTIME=${STATIC_RUNTIME}
    cmake --build ${BUILD_DIR} -j${JOBS}
"

echo ">>> Done: ${BUILD_DIR}/caneo"
