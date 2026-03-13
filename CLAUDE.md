# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# About the project
A CAN bus visualization and send tool written in C++23.

# Build system
- CMake + Conan 2
- All dependencies are managed via Conan 2 (no system-installed libs)
- Key dependencies: `boost::asio` (async socket I/O), `dbcppp` (DBC decoding), `ftxui` (TUI), `yaml-cpp` (config)

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

## Config (`src/config/`)
- `Config` — top-level struct: `bool virtual_can` + `std::vector<InterfaceConfig>`
- `InterfaceConfig` — per-interface: `name`, `dbc` (path, may be empty), `baudrate` (0 = not set)
- Supports two YAML formats:
  - Old: `interfaces: { vcan0: dbc/file.dbc }` (scalar value)
  - New (`caneo.yaml`): top-level `virtual:` flag + per-interface map with `dbc` and `baudrate`

## Interface setup (`src/setup/`)
- `setup_interfaces(Config)` — called at startup before sockets open
- `virtual: true`: loads `vcan` kernel module, creates missing vcan interfaces, brings them up
- `virtual: false`: sets CAN bitrate and brings up real interfaces (skips if already up)
- Uses `if_nametoindex()` + `/sys/class/net/<name>/operstate` for existence/state checks

## Socket hierarchy (`src/socket/`)
- `Socket` — base class for any incoming data source
- `SocketCAN` — derived from `Socket`; reads from a Linux SocketCAN interface asynchronously via `boost::asio`
  - `CAN_RAW_RECV_OWN_MSGS` is enabled so self-sent frames appear in the trace
  - Has `send(uint64_t id, const std::vector<uint8_t>& data)` for transmitting frames

## Frame hierarchy (`src/frame/`)
- `DataFrame` — base class; holds a raw `Payload` and a decoded representation (list of `DecodedSignal`: name, value, unit)
- `CanFrame` — derived from `DataFrame`; adds a `Header` (CAN ID, interface name, DLC)
- Decoding (via `dbcppp`) populates the decoded representation inside `DataFrame`

## Decoder (`src/decoder/`)
- `Decoder` — abstract interface
- `DbcPppWrapper` — implements `Decoder` using `dbcppp`; loads a DBC file, decodes frames by CAN ID
- `DecoderRegistry` — maps interface name → `Decoder`; used during receive to decode incoming frames

## Send model (`src/send/`)
- `SendModel` — loads a DBC file independently of the decoder; provides:
  - Sorted list of `SendMessage` (by CAN ID), each with sorted `SendSignal` entries (by start bit)
  - `set_value(msg_idx, sig_idx, value)` — updates a signal's physical value
  - `encode(msg_idx)` → `vector<uint8_t>` — encodes all signals into raw CAN bytes via `dbcppp`

## Action system (`src/action/`)
- `Action` — base class; holds interface, msg ID/name, payload, a `boost::asio::steady_timer`, and a `SendFn`
  - `SingleAction` — fires once immediately (timer with 0ms delay), then calls `on_done`
  - `PeriodicAction` — fires every N ms, reschedules itself after each fire
- `ActionHandler` — manages a list of actions; all list mutations are posted to the `io_context` thread
  - `add_action(unique_ptr<Action>)` — thread-safe, posts to asio thread
  - `remove_action(idx)` — thread-safe, posts cancel + erase to asio thread
  - `snapshot()` → `vector<ActionInfo>` — thread-safe read for the TUI (protected by mutex)
  - Calls a `notify_` callback after any change so the TUI re-renders
- `SendFn = std::function<void(iface, id, data)>` — type alias used throughout

## TUI (`src/gui/`)
- `TuiDataFrameSet` — full-screen ftxui application; three main tabs:
  - **Trace** (`t`): live view of received frames per interface, sub-tabs per interface (←/→)
  - **Send** (`s`): browse DBC messages and edit signal values before sending
  - **Actions** (`a`): list of active SingleActions and PeriodicActions

### TUI navigation
| Level | `nav_level_` | Controls |
|-------|-------------|---------|
| Main tab bar | 0 | `←`/`→` switch tabs; `↓` go to sub-tabs; `t`/`s`/`a` jump directly |
| Sub-tabs / Action list | 1 | `←`/`→` switch interface tabs; `↑` back to main tabs; `↓` (Send) into message list |
| Message list | 2 | `↑`/`↓` navigate; `→` enter signal view; `←` back to sub-tabs |
| Signal list | 3 | `↑`/`↓` navigate (signals + action buttons below); `→` edit/activate; `←` back to messages |

### Send / Signal view
- Signal rows: `→` enters edit mode (type value, `←`/`Enter` confirm, `Esc` cancel)
- Below signals (with spacing): `[ Single Action ]` and `[ Periodic Action ]` buttons
  - `→` on Single Action: creates a `SingleAction` immediately
  - `→` on Periodic Action: opens inline period input (digits only, ms); `←`/`→`/`Enter` confirm

### Actions tab
- Shows: ID, name, type, period, interface, last sent time ("Xms ago" / "never")
- `Del`/`Backspace`: removes selected action
- Actions update live (timer fires trigger `PostEvent` via `ActionHandler::notify_`)

## Threading model
- **asio thread**: runs `io_context`; handles socket reads, timer callbacks, all `ActionHandler` mutations
- **ftxui thread** (main thread): renders TUI, handles keyboard events
- `sets_` (trace data) guarded by `mutex_` (written by asio thread, read by ftxui thread)
- `ActionHandler::snapshot_` guarded by `snapshot_mutex_` (written by asio thread, read by ftxui thread)
- `ActionHandler` action list (`actions_`) only accessed on the asio thread (via `boost::asio::post`)
