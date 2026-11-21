# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# About the project
A CAN bus visualization and send tool written in C++23.

# Build system
- CMake + Conan 2; details in `docs/building.md`
- Dependencies via Conan 2 (no system-installed libs), except **Dear ImGui (docking branch) + ImPlot**,
  which `cmake/ImGui.cmake` fetches with FetchContent (conan's imgui has no docking)
- Key dependencies: `boost::asio`, `dbcppp`, `ftxui`, `yaml-cpp`, `sol2`/Lua, gRPC, mcap, imgui/implot/glfw,
  `elfio` + `libdwarf` (XCP: ELF symbols / DWARF types)
- dbcppp does not build with GCC >= 14 -> build in Docker (Ubuntu 24.04, GCC 13)

## Build commands
```bash
./docker-build.sh                 # build/caneo (conan cache in docker volume caneo-conan-amd64)
./docker-build.sh --arch arm64    # build-arm64/caneo via QEMU
```
- Headless build without GLFW/OpenGL/ImGui: conan `-o "&:gui=False"` + CMake `-DCANEO_GUI=OFF` (`CANEO_HAS_GUI=0`,
  no `run_gui`, default mode = headless instead of GUI). GUI-only sources are listed in the `if(CANEO_GUI)` block of CMakeLists.txt

## Raspberry Pi 5 image (`plat/rpi5/`, see its README.md)
- Yocto scarthgap (GCC 13 / glibc 2.39) in Docker (`caneo-yocto`), layers pinned in `layers.txt`, own layer `meta-caneo`
- `build.sh sdk` (meta-toolchain) -> `build.sh caneo` (conan profile `rpi5` from the SDK env, headless cross build) ->
  `build.sh image` (`caneo-image`, binary from `meta-caneo/recipes-apps/caneo/files/caneo`)
- PCAN-M.2 = `peak_pciefd` (kernel fragment `can-peak.cfg`, `dtparam=pciex1` + `dtoverlay=pcie-32bit-dma-pi5`, else DMA alloc fails with -12); can0/1/3 CAN 1M, can2 FD 1M/5M via
  systemd-networkd; eth 192.168.10.75/24; user can/can; `caneo.service` = `--headless --grpc_server` (port 50051)

# C++ conventions
- C++23, use modern features
- Every class implements both:
  - `operator<<` for `std::ostream`
  - A `std::formatter<T>` specialization with `parse`/`format` methods delegating to `operator<<` via `std::ostringstream`, so that `std::println("{}", myObj)` works
  - Do NOT use `std::ostream_formatter` — it is not available in GCC 15

# Architecture

## Config (`src/config/`)
- `Config` — `virtual_can`, `log_file_path` (raw, `log_dir()` expands `~`), `GuiConfig gui` (scale, theme),
  `std::vector<InterfaceConfig>`, `std::vector<GraphConfig> graphs`
- `InterfaceConfig` — `name`, `dbc` (may be empty), `baudrate` (0 = not set), `std::vector<ActionSpec> actions`
- `ActionSpec` — serializable action description (type single/periodic/sin/lua, msg, signals, period, sin params, script, paused)
- `load_config()` / `save_config()` — one YAML file holds everything; legacy `graphs_preset:` and
  `actions: <file>` are still read and merged in
- `AppConfig` — CLI flags + `config_path`; `restart_args()` for the GUI's re-exec

## Input pipeline (`src/config/Pipeline.*`, `src/gui/PipelineWindow.*`) — graphical channel setup
- Config `pipeline:` (nodes + links, `PipelineSpec`) is the source of truth; `Config::interfaces` (flat list used by sockets,
  decoders, TUI) is **derived** from it (`derive_interfaces()`, called after loading via `sync_pipeline()` and after edits).
  Old files with only `interfaces:` get a graph generated (`pipeline_from_interfaces()`); `interfaces:` is still written for compatibility
- Blocks: `hw_socketcan` (USB/PCI adapter from `detect_can_devices()` in `src/setup/CanDevices.*`, one output port per channel),
  `virtual_socketcan` (vcan names), `grpc_socketcan` (remote channels of a caneo `--grpc_server`, see "Remote channels" below), `baudrate` (bit/s or `virtual`, `fd` + `data_baudrate` = CAN FD), `decode` (DBC), `live` (trace/graphs),
  `log` (format mcap|blf), `xcp` (XCP master: name, `tx_id`/`rx_id`, `addr_ext`, `timeout_ms`, `pad`; input only)
- `xcp` blocks are derived into `Config::xcp_targets` (`XcpTargetConfig`, fd from the channel); XCP parameter changes need no restart
- Per interface derived: `baudrate`, `virtual_can`, `dbc`, `live` (reaches a Live block), `log` (reaches a Log block); channels that
  are connected to nothing are dropped. `logging.format` follows the fed Log blocks (mcap / blf / both). `run_gui` only forwards frames
  to the GUI / log when the flag is set; `setup_interfaces()` creates vcan per interface (`virtual_can`)
- GUI: Settings tab "Channels" (`PipelineWindow::render_contents`, imnodes, own context + editor); channel changes need a restart (banner with Save & Restart)

## Remote channels (gRPC SocketCAN)
- Config `grpc: {enabled}` (`GrpcConfig`, Settings > General > "Remote (gRPC)"); only then the block `grpc_socketcan` is offered
  and derived. Block: `address` (host:port), `prefix`, `channels` = selected server channel names; no baudrate block (server owns the bus)
- Derived `InterfaceConfig::grpc` (address) + `remote` (server name), local `name` = prefix + remote; written to `interfaces:` too.
  `plan_setup()` skips them; GUI/TUI/headless open them as `SocketGrpc(io, name, grpc, remote_name())` (also next to local channels)
- `ListInterfaces` RPC (`CanStreamService::set_interfaces()` = local channels + operstate, host name); client
  `list_remote_interfaces()` (`src/grpc/RemoteInterfaces.*`), PipelineWindow queries it async per block (Query button / address edit)
- "Reset HW" (menu bar next to Stop, `Gui::render_reset_button`, `set_reset_control`): `reset_interfaces()` (InterfaceSetup) takes
  channels down + up via SIOCSIFFLAGS (controller restart, bitrate stays, open sockets keep working); without CAP_NET_ADMIN one
  `run_privileged()`. Remote channels: `ResetInterfaces` RPC (`reset_remote_interfaces()`), server resets only its own channels, never prompts
- `RawCanFrame` carries `fd` / `brs`. The old global `--grpc_client=host:port` (all channels remote) still works

## Start-up modes (`src/main.cpp`, `AppConfig`)
- No flag = GUI; `--tui` = terminal UI; `--headless` = the former no-UI default (print/log). Without a config file (no `caneo.yaml`, no
  `--config`, no interface args) the GUI opens the modal "Select configuration" (`Gui::render_welcome`): recent configs
  (`src/config/RecentConfigs.*`, `~/.config/caneo/recent_configs`, fed by every load/save), Browse..., or "Blank config"; picking one restarts with it.
  The TUI shows a directory/file picker instead (`src/modes/config_picker.*`)
- GUI: nothing is set up or opened at start. Menu bar "Start" runs `plan_setup()` + `run_privileged()` (ONE pkexec/sudo call, none if
  nothing has to change) on a worker thread and then opens the sockets on the asio thread; "Stop" closes them (`run_gui.cpp`, `Gui::render_channel_button`).
  `--log` starts the channels immediately. TUI / headless still call `setup_interfaces()` at start
- GUI modes: Live / Debug / DBC / XCP / Settings (`Gui::Mode`); Settings fills the work area (`SettingsWindow::render_full`)

## Interface setup (`src/setup/`)
- `plan_setup(Config)` -> privileged commands; `run_privileged()`; `setup_interfaces(Config)` = both for the CLI modes
- `virtual: true`: loads `vcan` kernel module, creates missing vcan interfaces, brings them up
- `virtual: false`: sets CAN bitrate and brings up real interfaces (skips if already up)
- Uses `if_nametoindex()` + `/sys/class/net/<name>/operstate` for existence/state checks

## CAN FD
- `InterfaceConfig::fd` / `data_baudrate`; `plan_setup()`: `ip link set X type can bitrate B dbitrate D fd on` (reconfigures an
  interface that is up with the wrong FD mode, detected by MTU 72 in `/sys/class/net/X/mtu`), vcan: `mtu 72`
- `SocketCAN` enables `CAN_RAW_FD_FRAMES`, reads `canfd_frame` (CAN_MTU / CANFD_MTU), `CanHeader::fd` / `brs`;
  `send()` sends an FD frame (BRS, padded to a valid length) for > 8 bytes or `fd = true`

## XCP (`src/xcp/`, `src/gui/XcpView.*`) — measure / calibrate an ECU (XCPlite etc.) over XCP on CAN (FD)
- `load_elf()` (`ElfReader`): ELFIO symbol table + libdwarf DIE walk -> `ElfImage` tree of `ElfVariable` (structs, unions,
  arrays (max 256 elements per dim shown), bitfields (`bit_offset` from the LSB of the storage unit), enums, function statics
  `func.var`, namespaces `ns::`); `by_path` = leaves. Without DWARF: object symbols, type guessed from the size. Move-only
- `load_a2l()` / `a2l_image()` (`A2lReader`): own A2L tokenizer + block parser; MEASUREMENT / CHARACTERISTIC (VALUE, VAL_BLK,
  ASCII, CURVE/MAP without axis points in the record layout), COMPU_METHOD (IDENTICAL, LINEAR, RAT_FUNC without p^2 ->
  `XcpConversion` phys = (p + q raw) / (s + t raw); TAB_VERB -> enumerators), COMPU_VTAB(_RANGE), RECORD_LAYOUT, UNIT, BIT_MASK,
  MATRIX_DIM, SYMBOL_LINK. `a2l_image()` builds an `ElfImage` (`from_a2l`, leaf path = A2L name, `description` = LONG_IDENTIFIER,
  tree split by `/` of the LONG_IDENTIFIER, else by `.` of the name); addresses from the ELF when loaded (name / SYMBOL_LINK, a
  stale ECU_ADDRESS is reported), else ECU_ADDRESS. `xcp_decode` / `xcp_encode` apply `XcpVar::conv`
- Simulink (MABY REALTIME): `CORE/model/model_build.m` builds with DefaultParameterBehavior=Tunable + OptimizeBlockIOStorage=off
  (not saved to the .slx) and writes `_BUILD/model_ert_rtw/model.a2l` (`coder.asap2.export`, LONG_IDENTIFIER = block path from the
  code descriptor, ECU addresses via MapFile from `_BUILD/linux/model` if it exists)
- `XcpCanTransport`: own raw socket per master (filter on the slave ID, FD frames, MTU check), blocking with poll()
- `XcpMaster`: worker thread (`std::jthread`), requests queued (connect, disconnect, set_measurements, write, read).
  CONNECT (byte order, MAX_CTO/DTO), GET_ID, GET_DAQ_PROCESSOR/RESOLUTION/EVENT_INFO. Polling = SHORT_UPLOAD every poll_ms;
  DAQ = one dynamic list per event (FREE/ALLOC_DAQ/ODT/ODT_ENTRY, WRITE_DAQ, SET_DAQ_LIST_MODE, START_STOP), first-fit ODT packing,
  all 4 identification field types, timestamps unwrapped (lists whose first ODT cannot hold anything next to the timestamp run
  without). Write = SET_MTA + DOWNLOAD (bitfields read-modify-write), then read back. Timeout -> SYNCH + retry.
  `SampleFn(name, t since connect, value)` runs on the worker; `xcp_decode` / `xcp_encode` convert values
- `XcpView` (`Gui::Mode::Xcp`): toolbar (target, connect, ELF + A2L load/reload async, poll ms; `vars()` = A2L image if loaded, else ELF), centre = stacked ImPlot plots (linked X,
  toolbar like `GraphWindow`: Live/Paused, window presets + wheel, Auto Y per plot (Y-axis zoom = manual), Fit, Default / double-click, cursor read-out;
  live window, drag & drop `CANEO_XCP_VAR`), right = measured table (value, poll / DAQ event) + clipped variable tree / filter
  (right-click: edit, read, plot, measure poll / every cycle (DAQ)), modal "Edit value" (enum combo, bool checkbox, hex)
- Config `xcp:` (`XcpConfig`: target, elf, a2l, poll_ms, measure [{name, event}], plots) via `AppState::set_xcp_snapshot()`;
  samples are logged as `xcp/<var>` through `AppState::log_scalar()`. `Gui::run` calls `XcpView::shutdown()` before returning
- Testing without hardware: `scripts/xcp_sim.py` (XCP slave on vcan, DAQ + timestamps, `--fd`, `--id-field`) and
  `build/caneo_xcp_test` (selftest incl. A2L parsing / ELF or A2L listing (`--a2l f [--elf f]`) / CLI master), test ELF from `tests/data/xcp_target.c`

## Socket hierarchy (`src/socket/`)
- `Socket` — base class for any incoming data source
- `SocketCAN` — derived from `Socket`; reads from a Linux SocketCAN interface asynchronously via `boost::asio`
  - `CAN_RAW_RECV_OWN_MSGS` is enabled so self-sent frames appear in the trace
  - RX timestamps via SO_TIMESTAMPING: hardware timestamps run on the adapter/driver clock (peak_usb: CLOCK_MONOTONIC),
    so they are mapped to the wall clock with `hw_offset_` (smallest now - hw seen, reset on > 1 s jumps); software ts = realtime
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
- Every `Action` carries its `ActionSpec` (empty type = ad-hoc, not saved) and a shared `ActionLog` (lines + error)
- `LuaAction` — calls `tick(t)` of a Lua script every period; API `send`, `send_raw`, `get_signal`, `print`;
  runtime errors pause the action
- `ActionFactory` — `ActionSpec` -> `Action`; caches one read-only `SendModel` per DBC path (used on the asio
  thread for encoding); `add_all()` loads the actions of a config
- `ActionHandler::replace_action(idx, action)` keeps the paused state; `clear()`, `set_all_paused()`

## Guards (`src/guard/`) — monitoring (TUI / headless only; the GUI runs guards as flows, see `src/flow/Legacy.*`)
- `GuardSpec` (config `guards:`) — name, interface, msg, signal, script, enabled
- `Guard` — own Lua state; `evaluate()` runs `check(value, t)` for every frame of msg (matched by
  interface + decoded msg name); outputs = returned number/bool/table; errors disable the guard
- `GuardState` — live input/outputs/count + `ActionLog` (print/error), mutex-protected, read by the GUI
- `GuardEngine` — list mutations posted to io (like `ActionHandler`), `on_frame()` on the asio thread,
  `set_output_fn()` -> GUI graph (`GraphWindow::GUARD_IFACE`, series "guard/output") + `log_scalar("guard/<name>/<output>")`
- Used in run_live (headless `--log`) and run_tui. `run_gui` / `AppState::load` convert `guards:` and non-single `actions:`
  into flows (`migrate_legacy()`, `src/flow/Legacy.*`); `AppState::collect()` no longer writes them back
- GUI: Trace signal context menu "Add guard..." creates a guard flow (`FlowsWindow::guard_template`)

## Flows (`src/flow/`) — block diagrams (config `flows:`), GUI only
- GUI: `FlowsWindow` ("Flows", left dock) lists the live flows (run/stop, status, duplicate/delete); a click opens the
  flow in `FlowEditorWindow`. Debug view shows its own offline flows as tabs
- Control blocks (live): `slider` (value goes into the running flow at once via `FlowEngine::set_control`, no Apply; value is saved), `button`
  (1 for one step, not sent to CAN), `rs` (RS flip-flop S/R -> Q, !Q, `op` R|S = dominant), `switch` (a, b, ctrl -> ctrl>0.5 ? b : a). Offline: slider = const, button = 0
- Extra blocks: `add`, `mul` (a * b), `sin` / `cos` (radians, or `degrees`), `gain` (in * gain), `integrator` ("1/s", forward Euler, start value = `offset`), `diff`, `compare`, `movavg`, `fft` (`math_block_step()`, shared by live + offline), `csv` (offline: N inputs -> table, "Export" writes `path`)
- `can_out`: `sig_values` = constants for unconnected inputs, `period_ms` 0 = send once on start, "Send now" (`FlowEngine::trigger`)
- `lua`: input names in `inputs` (several -> `step(in1, in2, ..., t)`; menu "Lua: Position" = vx, vy, yaw rate -> X, Y, psi), output names in `signals` (step returns a table by name/position), `send()`/`send_raw()` (live), optional `init()`
- Scope names with `/` form sub-folders in the Debug signal tree
- `FlowSpec` (nodes + links, `[from_node, from_port, to_node, to_port]`), node types: `can_in` (one output port per
  selected signal), `can_out` (one input port per signal, sends the message every `period_ms`), `lua` (1 in / 1 out,
  `step(u, t)`, own Lua state per block), `generator` (sin/pulse/ramp/sawtooth/triangle/noise/const), `scope`
  (-> graph series `flow/<flow>/<scope>` + `log_scalar`)
- `Flow` — steady_timer every `step_ms`, blocks evaluated in dependency order; loops read the previous step;
  unconnected `can_out` inputs keep the DBC default. `FlowEngine` — like `GuardEngine` (posts to io, snapshot for the GUI);
  flows are built on the GUI thread (`ActionFactory::model_for` is not thread-safe)
- Navigation of every imnodes editor (`src/gui/ImNodesPan.hpp`, `ImNodesZoom`): wheel = zoom (emulated: window font scale + scaled ImGui/imnodes style + node
  positions; stored positions stay unscaled), right-drag / middle / Alt+left-drag = pan, Shift+wheel sideways, toolbar -/+/100%
- `FlowEditorWindow` — imnodes (fetched in `cmake/ImGui.cmake`), edits a working copy, "Apply" (re)starts the flow;
  right-click on the canvas adds blocks, Delete removes the selection, live values are shown at the ports

## TUI (`src/gui/`)
- `TuiDataFrameSet` — full-screen ftxui application; main tabs (`Tab` enum, `tabs()` = visible ones):
  - **Trace** (`t`): live view of received frames per interface, sub-tabs per interface (←/→)
  - **Send** (`s`): browse DBC messages and edit signal values before sending
  - **Actions** (`a`): list of actions; `→` on a Lua action shows its print()/error log
  - **Guards** (`g`, only after `set_guard_engine()`): live input/outputs/count, Space = enable/disable, `→` = log
  - **Graphs** (`v`): `GraphView`s (from `set_graphs(config.graphs)`), series recorded in `update()` /
    `push_model_output()` / `push_guard_output()` under `mutex_`; drawn with `canvas()` (braille, min/max per column)
  - **Model** (`m`) / **Playback** (`p`) when enabled
- Mouse: render functions wrap elements in `clickable()` / `button()`, which register a `ClickTarget` (box via
  `reflect`); `handle_mouse()` runs the smallest box containing the click, the wheel calls `scroll()`.
  Buttons mostly `emit()` the equivalent key event through `key_handler_`, so mouse and keyboard share one code path.
  `render_*` are non-const because they register targets; `click_targets_` is cleared at the start of `render()`
- Logging via `set_log_control(ILogControl*, const Config*)`: `l` opens a path prompt (status line at the
  bottom) or stops the log, `o` opens it in `log_viewer`; REC timer in the title. A 500 ms ticker redraws
  without traffic. `LogController::set_verbose(false)` in run_tui - stdout belongs to ftxui.

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

## GUI (`src/gui/`, Dear ImGui docking)
- `Gui` — GLFW/OpenGL loop, main menu bar (File/View/Actions + log/gRPC status), `DockSpaceOverViewport`,
  default layout via `DockBuilder` when `imgui.ini` has no `[Docking]` section
- `Window` base: `title()` (= ImGui window name / dock id) + `visible`; every window calls `Begin(title(), &visible)`
- `AppState` — running config vs. edited config, `collect()` (config + live flows + live graphs); owns no actions/guards (GUI = flows only), `ModelCache` gives DBC models, `send()` posts a frame to the asio thread, `save()`, `load()`
  (applies graphs/actions/gui live; channel changes need restart), `request_restart()` -> `run_gui` returns true -> `main` execs
- `GuiStyle` — fonts (embedded Roboto/Cousine, see `cmake/EmbedFile.cmake`) rebuilt at `16px * scale`, style scaling, theme
- Windows: `TraceContainerWindow`/`TraceWindow`, `GraphWindow` (ring buffer + min/max decimation, Live/Paused,
  Auto-Y, Fit, Default), `SendWindow` (sends a frame directly; "Periodic as flow" creates a CAN Out flow), `FlowsWindow`,
  `SettingsWindow`, `ModelWindow`; `FileBrowser` is a small modal file dialog (call `render()` in the same ID scope as `open()`)

## Debug mode (`src/gui/DebugView.*`, `src/debug/LogDocument.*`)
- `Gui::Mode` Live/Debug, 2-segment toggle centred in the menu bar (`render_mode_toggle()`), default Live. In Debug the
  dockspace/live windows are skipped and `DebugView` fills the whole work area (offline MCAP/BLF viewer, PlotJuggler-like)
- `LogDocument` loads a log on a worker thread into `Series` (t in s since first sample, v); `poll()` on the render thread.
  MCAP: decoded `/data` channels via `McapReader::read_signals()` (unpaced). BLF: raw frames kept, decoded with one DBC per
  interface (`decode()` re-decodes with other DBCs; DBC defaults come from the config interfaces)
- `DebugView`: sidebar (open, interfaces/DBC, filterable signal tree, drag & drop / double-click) + stacked ImPlot subplots
  with linked X, shared cursor + value tooltip, min/max decimation of the visible range, legend right-click = remove
- History: `AppState::save()` / autosave store `DebugView::snapshot()` (log, DBCs, tabs/plots as series labels) as
  `DebugSession` in `config.debug_history` (newest first, max 20, one per log path, config `debug_history:`).
  Sidebar "History" button + Debug > History menu -> `DebugView::restore()` reloads log and rebuilds the plots
- Offline flows (`config.debug_flows`, `src/debug/OfflineFlow.*`): `FlowEditorWindow` got a `FlowBackend` (live = FlowEngine,
  offline = `OfflineFlowBackend` in DebugView.cpp), `render_contents()` and `set_compact()` so it can be embedded. The sidebar
  has a flow list below the signal tree (draggable divider); clicking a flow opens a "Flow: <name>" tab (own editor instance,
  addressed by flow index -> deleting a flow closes all flow tabs) next to the plot tabs. All flows always run (no enable flag):
  `run_offline_flow()` steps a FlowSpec on a `step_ms` grid over the loaded log (can_in = sample-and-hold of the log signals,
  lua/generator as live, can_out ignored); each scope becomes series `flow/<flow>/<scope>`, appended after the log's own series
  (`LogDocument::set_extra_series()`, `Data::base_series`). Flows re-run on every change and after (re)loading a log
- XY plot: tab button "New XY plot" -> tab with ONE ImPlot (`PlotTab::xy`, `render_xy`): exactly two dropped signals, first = X, second = Y (linearly resampled onto the X sample times); stored as `xy: true` in the history tab
- Plot close: small "x" in the corner of each plot when a tab has more than one

## DBC mode (`src/gui/DbcEditor.*`, `src/dbc/DbcModel.*`)
- Third mode next to Live / Debug (`Gui::Mode::Dbc`): `DbcEditor` fills the work area. Left: message list (filter) and node list;
  right: message (ID, extended, size, sender node), bit layout grid (click = select signal, red = overlap), signal table,
  signal editor (start/size/order/sign, factor/offset/min/max, unit, mux, receivers, value table)
- "Network" view (`DbcNetwork`, own ImDrawList canvas, no imnodes): boxes = nodes (move, resize via corner), arrows = messages sender -> receivers,
  derived from the DbcDoc; links dock on the side facing the other node. Drag from a box border onto another box creates a message
  (default signal carries the receiver) or extends an existing one; double click = in/out dialog with the pool of sender-less messages.
  Box positions/sizes are stored in `<dbc>.net` next to the DBC (written by Save)
- History: recently opened / saved DBC files in `config.dbc_history` (config `dbc_history:`, newest first, max 20); toolbar "History" button + DBC > History menu
- `DbcDoc` is an own parser/writer (BU_, BO_, SG_, CM_, VAL_, VERSION; BA_* lines are kept verbatim), independent of dbcppp;
  `problems()` reports duplicates, overlaps (multiplexed signals may share bits) and out-of-range signals

## Logging (GUI)
- `ILogControl::start(path)` / `supports_path()` / `last_path()`; `LogController` supports a custom path,
  `RemoteLogControl` (gRPC) ignores it (server picks the file)
- "Start log..." dialog in `Gui::render_log_dialog()`; `Config::log_viewer` command (`{file}` placeholder)
  started detached via `log_viewer::launch()` (`src/gui/LogViewer.*`), `detect_plotjuggler()` scans ~/Downloads etc.

## BLF
- `BlfWriter` / `BlfReader` (`src/logger/`, constants in `BlfFormat.hpp`) follow python-can's `can/io/blf.py`:
  144-byte "LOGG" header, LOBJ log containers (zlib, 128 KiB), CAN_MESSAGE / CAN_FD_MESSAGE objects,
  APP_TEXT "caneo-channel:<n>=<iface>" for the interface mapping
- `LogReader` interface (`open_log_reader()` detects BLF by the "LOGG" magic) implemented by `McapReader` and `BlfReader`;
  `run_playback` decodes BLF frames via `set_decoder()` + DecoderRegistry
- `LogController` writes MCAP and/or BLF (`LogOptions::format`); TUI also logs through LogController
- `SocketCAN::send` sets `CAN_EFF_FLAG` for IDs > 0x7FF (received IDs carry no flags)

## Log options
- `LogOptions` (config `logging:`): raw, update_ratio, min_interval_ms (per-ID thinning in `McapLogger::log`),
  flush_s (`closeLastChunk()` + flush) - all applied inside `McapLogger`, so every mode honours them
- `update_ratio` is computed on every frame by the caller (LogController / log_frame) before thinning
- GUI: Settings > Logging edits `config().logging`; `Gui::run` pushes changes via `ILogControl::set_options`
- Config autosave: `GuiConfig::autosave_s`, `AppState::autosave_tick()` compares `config_to_yaml(collect())`

## Threading model
- **asio thread**: runs `io_context`; handles socket reads, timer callbacks, all `ActionHandler` mutations
- **ftxui / ImGui thread** (main thread): renders TUI or GUI, handles input; `ActionFactory` and `AppState` live here
- `SignalStore` and `LuaAction` Lua states are only touched on the asio thread
- `sets_` (trace data) guarded by `mutex_` (written by asio thread, read by ftxui thread)
- `ActionHandler::snapshot_` guarded by `snapshot_mutex_` (written by asio thread, read by ftxui thread)
- `ActionHandler` action list (`actions_`) only accessed on the asio thread (via `boost::asio::post`)
