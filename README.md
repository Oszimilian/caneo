# caneo

A CAN bus visualization tool written in C++23.

## Requirements

- CMake >= 3.25
- Conan 2
- GCC >= 14 or Clang >= 17 (C++23 required)

## Build

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

# Listen on multiple interfaces simultaneously
./build/caneo vcan0 vcan1

# Send a test frame
cansend vcan0 123#DEADBEEF

# Send frames continuously
cangen vcan0
```
