# Yocto scarthgap build host (Ubuntu 22.04 is an officially supported host).
# Ubuntu 24.04 on the host restricts unprivileged user namespaces (AppArmor),
# which bitbake needs, so the build runs in this container.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    gawk wget git diffstat unzip texinfo gcc g++ build-essential chrpath socat cpio \
    python3 python3-pip python3-pexpect xz-utils debianutils iputils-ping python3-git \
    python3-jinja2 python3-subunit zstd liblz4-tool file locales libacl1 lz4 \
    ca-certificates sudo cmake pkg-config bmap-tools \
    && rm -rf /var/lib/apt/lists/* \
    && locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
# conan for the caneo cross build with the Yocto SDK
# (CMake >= 3.25 from pip, Ubuntu 22.04 only has 3.22)
RUN pip3 install "conan>=2.0,<3" "cmake>=3.27,<4"
ARG UID=1000
ARG GID=1000
RUN groupadd -g ${GID} build && useradd -m -u ${UID} -g ${GID} -s /bin/bash build \
    && echo "build ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/build
USER build
WORKDIR /work
