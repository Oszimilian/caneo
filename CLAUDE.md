# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# About the project
A CAN bus visualization tool written in C++23.

# Build system
- CMake + Conan 2
- All dependencies are managed via Conan 2 (no system-installed libs)
- Key dependencies: `boost::asio` (async socket I/O), `dbcppp` (DBC decoding)

## Build commands
```bash
conan install . --output-folder=build --build=missing
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

# C++ conventions
- C++23, use modern features
- Every class implements both:
  - `operator<<` for `std::ostream`
  - A `std::formatter<T>` specialization with `parse`/`format` methods delegating to `operator<<` via `std::ostringstream`, so that `std::println("{}", myObj)` works
  - Do NOT use `std::ostream_formatter` — it is not available in GCC 15

# Architecture

## Socket hierarchy
- `Socket` — base class for any incoming data source
- `SocketCAN` — derived from `Socket`, reads from a Linux SocketCAN interface (can0, can1, …) asynchronously via `boost::asio`
- The base class exists because future sources (e.g. UDP/TCP sockets) should fit the same interface

## Frame hierarchy
- `DataFrame` — base class for any received frame; holds a raw `Payload` (allocated buffer) and a decoded representation (list of key-value entries: signal name → value with unit)
- `CanFrame` — derived from `DataFrame`; adds a `Header` containing: CAN ID, interface name (can0/can1/…), and DLC
- Decoding (via `dbcppp`) populates the decoded representation inside `DataFrame`, not in a subclass

## Visualization
- Terminal output for now (via `operator<<` / `std::println`)
- ImGui (dear imgui) is planned for later — do not design around it yet
