# caneo

A CAN bus visualization and send tool written in C++23.

Supports live CAN bus monitoring, DBC decoding, signal sending, action scripting, Lua model evaluation, MCAP logging, and gRPC-based remote streaming.

See [Architecture](docs/architecture.md) for the class diagram and detailed design notes.

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
conan install . --output-folder=build --build=missing -c tools.system.package_manager:mode=install
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

> **Note:** The `-c tools.system.package_manager:mode=install` flag is required because `glfw` depends on `xorg/system`, which needs X11 dev packages to be installed automatically.

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

## Modes

| Flag | Description |
|------|-------------|
| *(none)* | CLI mode — print decoded frames to stdout |
| `--tui` | Interactive terminal UI (trace, send, actions, model) |
| `--gui` | Dear ImGui graphical UI |
| `--playback <file.mcap>` | Replay a recorded MCAP file |

## gRPC streaming

caneo can stream raw CAN frames over gRPC, allowing a remote instance to receive and display frames as if they were local.

**Server** — runs normally on real CAN hardware, additionally streams all frames to connected clients:
```bash
./build/caneo --grpc_server --tui
./build/caneo --grpc_server --gui
./build/caneo --grpc_server           # headless
```

**Client** — connects to a server and receives frames via gRPC instead of reading from SocketCAN:
```bash
./build/caneo --grpc_client=192.168.1.42:50051 --tui
./build/caneo --grpc_client=192.168.1.42:50051 --gui
```

- The client does not require physical CAN hardware — `setup_interfaces` is skipped
- Interface names in `caneo.yaml` are used as subscription filters
- The client automatically reconnects if the server restarts
- Sending frames from the client (via Send tab / Actions) transmits them back to the server, which puts them on the real CAN bus
- A connection status indicator (● green/red) is shown in the top-right corner of both TUI and GUI

Additional gRPC flags:

| Flag | Default | Description |
|------|---------|-------------|
| `--grpc_port` | `50051` | Port the gRPC server listens on |

## Lua model

A Lua script can be run on every received frame to compute derived values:

```bash
./build/caneo --model path/to/script.lua vcan0:vcan0.dbc
```

The script must define a `run()` function that returns a number or a `{key=value}` table. Outputs are displayed in a dedicated **Model** tab in the TUI and as a separate window in the GUI.

## Other flags

| Flag | Description |
|------|-------------|
| `--log` | Write decoded frames to a timestamped MCAP file |
| `--debug` | Print raw frame data to stdout (CLI mode) |
| `--config <file>` | Load interface config from a YAML file instead of `caneo.yaml` |
