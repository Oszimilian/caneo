# caneo

A CAN bus visualization tool written in C++23.

See [Architecture](docs/architecture.md) for the class diagram.

## Requirements

- CMake >= 3.25
- Conan 2
- GCC >= 14 or Clang >= 17 (C++23 required)

## Build

### With Docker (recommended)

Due to a known build issue with `dbcppp` on GCC >= 14, the recommended way to build is via Docker (Ubuntu 24.04 / GCC 13):

Build the image once:
```bash
docker build -t caneo-builder .
```

Then build the project:
```bash
docker run -it -v .:/build/:Z caneo-builder
```

The binary is located at `build/caneo`.

For an interactive shell inside the container:
```bash
docker run -it -v .:/build/:Z caneo-builder bash
```

> **Note:** The `:Z` flag is required on Fedora/RHEL due to SELinux.

### Natively

Requires CMake >= 3.25, Conan 2, and GCC 13 (GCC >= 14 is currently broken due to a `dbcppp` upstream issue).

Install Conan if not already present:
```bash
pip install conan
conan profile detect
```

Then build the project:
```bash
conan install . --output-folder=build --build=missing
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Debug
```bash
conan install . --output-folder=build --build=missing --settings=build_type=Debug   
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

The binary is located at `build/caneo`.

## Virtual CAN interface

To test without real hardware, set up a virtual CAN interface:

```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0
```

To remove it afterwards:
```bash
sudo ip link delete vcan0
```

Run caneo against one or more interfaces, then send test frames using `can-utils`:
```bash
# Install can-utils if needed (Fedora/RHEL)
sudo dnf install can-utils

# Listen on one interface
./build/caneo vcan0

# Listen with DBC decoding
./build/caneo vcan0:vcan0.dbc

# Listen on multiple interfaces, with or without DBC
./build/caneo vcan0:vcan0.dbc vcan1

# Send a test frame
cansend vcan0 123#DEADBEEF

# Send frames continuously
cangen vcan0
```
