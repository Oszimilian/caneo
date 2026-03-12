FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    gcc-13 g++-13 \
    cmake \
    python3 python3-pip \
    && rm -rf /var/lib/apt/lists/*

RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
 && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100

RUN pip3 install conan --break-system-packages

RUN conan profile detect --force

WORKDIR /build

CMD ["bash", "-c", \
    "conan install . --output-folder=build --build=missing && \
     cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release && \
     cmake --build build"]
