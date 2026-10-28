# caneo

A CAN bus visualization and send tool written in C++23.

Supports live CAN bus monitoring, DBC decoding, signal sending, action scripting, Lua model evaluation, MCAP logging, and gRPC-based remote streaming.

See [Architecture](docs/architecture.md) for the class diagram and detailed design notes.

## Build

The recommended way is the Docker build script. It needs only Docker on the host, caches all
Conan dependencies in a volume, and can also build for **arm64**:

```bash
./docker-build.sh                 # -> build/caneo
./docker-build.sh --arch arm64    # -> build-arm64/caneo  (via QEMU)
./docker-build.sh --debug         # -> build-debug/caneo
```

Native builds need GCC 13 (dbcppp does not compile with GCC >= 14), CMake >= 3.25 and Conan 2.
Dear ImGui (docking branch) and ImPlot are fetched by CMake.

**All details are in [docs/building.md](docs/building.md)** (Docker internals, ARM, native build,
offline builds, ImGui updates).

## Configuration (`caneo.yaml`)

Everything lives in one file: CAN channels, GUI settings, actions and graphs. The GUI can load and
save it at runtime (File menu / Settings), so the usual workflow is to set things up in the GUI and
press *Save*.

```yaml
virtual: false                 # true: create vcan interfaces at start-up
log_file_path: ~/Dokumente/Logs    # default directory for "Start log..."
log_viewer: "~/Downloads/PlotJuggler-3.17.2-x86_64.AppImage -n -d {file}"
gui:
  scale: 1.25                  # GUI scale factor
  theme: light                 # light | dark
  autosave_s: 10               # save this file every 10 s if something changed (0 = off)
logging:
  format: mcap                 # mcap | blf | both
  raw: true                    # <iface>/<msg>/raw - original bytes, needed for --playback
  update_ratio: false          # <iface>/<msg>/update_ratio - time since previous frame
  min_interval_ms: 0           # log each CAN ID at most every N ms (0 = every frame)
  flush_s: 10                  # write to disk every N s (crash / power-cut safety)

interfaces:
  can0:
    dbc: /path/to/vehicle.dbc
    baudrate: 1000000
    actions:
      - type: periodic         # periodic | sin | lua | single
        msg: SS_ELMO_STATE
        period_ms: 1
        signals:
          SS_ELMO_RUN: 1
        paused: true           # state when loaded
      - type: sin
        msg: SS_ELMO_TARGET
        signal: SS_ELMO_TARGET_LWS
        amplitude: 2000
        offset: 6000
        sin_period_ms: 2000
        period_ms: 1
      - type: lua
        script: scripts/steering_sweep.lua
        period_ms: 10

guards:                        # watch signals with Lua, see "Guards" below
  - name: lws_check
    interface: can0
    msg: SS_ELMO_TARGET
    signal: SS_ELMO_TARGET_LWS
    script: scripts/lws_guard.lua
    enabled: true

graphs:
  - name: Lenkung
    window_s: 30               # default time window
    auto_y: true
    signals:
      - msg: RFC4800_LWS
        signal: LWS_Sensor
      - interface: can0        # optional, otherwise all interfaces are searched
        msg: SS_ELMO_TARGET
        signal: SS_ELMO_TARGET_LWS
        y_axis: 1              # 0 = Y1 (left), 1 = Y2, 2 = Y3
```

The older layout with separate files is still read: `graphs_preset: file.yaml` and per-interface
`actions: file.yaml`. Saving from the GUI writes everything into the single file. Comments are not
preserved.

## Log size

Every logged MCAP message costs its compressed payload **plus 16 bytes of uncompressed message
index**. The index is required for reading in time order (PlotJuggler, `--playback`). By default
each CAN frame produces three messages (`data`, `raw`, `update_ratio`), so the index makes up about
60 % of the file. A busy bus with ~4000 frames/s gives about **1.2 GB/h**.

Measured on vcan at ~1 kHz for 10 s:

| `logging:` settings | size |
|---|---|
| everything (default) | 100 % |
| `update_ratio: false` | 63 % |
| `raw: false` | 69 % |
| `raw: false`, `update_ratio: false` | 31 % |
| plus `min_interval_ms: 10` (max. 100 Hz per ID) | 6 % |

Your own periodic actions are logged too (`CAN_RAW_RECV_OWN_MSGS`). An action with `period_ms: 1`
alone adds 1000 frames/s.

### MCAP or BLF

| | MCAP | BLF (Vector Binary Logging Format) |
|---|---|---|
| Content | decoded signals, guard/model outputs, optional raw + update rate | raw CAN / CAN FD frames only |
| Size (same 1 kHz test) | 211 kB (data only) ... 687 kB (everything) | **59–69 kB** |
| Open with | PlotJuggler, Foxglove | CANalyzer/CANoe, asammdf (+ DBC), python-can |
| DBC | applied while recording | applied when analysing, so a newer DBC also works on old logs |
| `--playback` | yes (needs `raw: true`) | yes, decoded with the DBCs from the config |

`format: both` writes `name.mcap` and `name.blf` side by side. BLF always contains every frame.
`raw`, `update_ratio` and `min_interval_ms` only affect the MCAP. A good setup for long recordings
is BLF as the complete archive plus a thinned-out MCAP for PlotJuggler.

caneo stores the interface names in the BLF (`APP_TEXT` "caneo-channel:1=can0"). For BLF files from
other tools, channel 1, 2, ... maps to the first, second, ... interface in the config.

`flush_s` also matters for safety: without it, a crash or power cut loses the **whole** log. With
`flush_s: 10` at most the last 10 s are lost, and the file stays readable.

## GUI (`--gui`)

The GUI uses dockable windows: drag a window by its title bar onto another window to dock it.
*View > Reset layout* restores the default layout.

- **Trace**: live frames per interface. Right-click a signal to add it to a graph or to put a guard
  on it. The **Guards** tab shows the input and outputs of every guard live; right-click an output
  to plot it.
- **Graphs**: one tab per graph (`+` adds one; right-click a tab to rename or delete it).
  - *Live / Paused*: follow the newest data, or freeze the view. Dragging pauses.
  - *Window*: visible time span (also the mouse wheel over the plot).
  - *Auto Y*: scale the Y axes to the visible data.
  - *Fit* shows all recorded data. *Default* (or double-click) returns to the live view.
  - Hovering shows the values of all signals at the cursor time.
- **Send**: pick a DBC message, edit its signals, send once, periodically or as a sine.
- **Actions**: browser for all actions. *+ Add action* opens the action editor (periodic, sine or Lua
  script). Click a row to edit it. Start/pause, delete.
- **Guards**: list of all guards (on/off, edit, delete) and the guard editor (interface, message,
  signal, script).
- **Script Editor**: edit Lua action and guard scripts (Ctrl+S saves). *Save & Reload actions* restarts every
  action that uses the script. The `print()` output and errors of those actions are shown below the
  editor.
- **Logging** (menu bar, right): *Start log...* asks for the file (pre-filled with log directory +
  timestamp, Enter starts it). Optionally the directory becomes the new default. *PlotJuggler* (or
  *View log*) opens the last finished log in the configured viewer.
- **Settings** (*File > Settings*): GUI scale and theme, load/save the config, logging (log directory,
  viewer command with `{file}` placeholder; PlotJuggler AppImages in `~/Downloads` are detected,
  *Open log file...*), CAN channels
  (interface, baudrate, DBC via a file browser, vcan). Channel changes take effect after
  *Save & Restart*.

### Guards (monitoring)

A guard watches one signal of one message. For **every received frame** of that message it calls
`check(value, t)` in its Lua script. The return value becomes the guard's outputs:

```lua
local limit = 7500
function check(value, t)               -- value = guarded signal, t = seconds since start
    return { too_high = value > limit, margin = limit - value }   -- booleans -> 1/0
end
```

- Return a number (output `output`), a boolean, or a table `{name = value}`.
- `get_signal(iface, msg, signal)`, `print(...)`, `init()` and the globals `interface`, `msg`,
  `signal` are available.
- The outputs are shown in **Trace > Guards** and can be plotted. While logging they are written to
  the MCAP as `guard/<guard>/<output>`. This applies in every mode, including headless
  `caneo --log` (the systemd service) and the TUI (tab **Guards**, key `g`).
- A runtime error disables the guard and shows the error. Enabling it again retries.
- See `scripts/lws_guard.lua`.

Guards are for watching signals. **Actions** are for sending.

### Lua actions

A Lua action calls `tick(t)` every `period_ms`:

```lua
function tick(t)
    send("SS_ELMO_TARGET", { SS_ELMO_TARGET_LWS = 6000 + 2000 * math.sin(2 * math.pi * t / 4) })
end
```

| Function | Description |
|----------|-------------|
| `send(msg, {SIG = value, ...} [, iface])` | Encode via DBC and send. Signals that are not listed keep their last value. |
| `send_raw(id, {b0, b1, ...} [, iface])` | Send raw bytes |
| `get_signal(iface, msg, signal)` | Last received value (0 if none) |
| `print(...)` | Write to the action log (shown in the Script Editor) |
| `init()` | Optional, called once after loading |
| `interface`, `period_ms` | Settings of the action |

A runtime error pauses the action and shows the error. *Start* tries again. See
`scripts/steering_sweep.lua`.

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
| `--tui` | Interactive terminal UI (trace, send, actions, guards, model, logging) |
| `--gui` | Dear ImGui graphical UI |
| `--playback <file.mcap\|file.blf>` | Replay a recorded MCAP or BLF file (format detected from the content) |

### TUI keys

Everything can also be done with the mouse: click tabs, rows and the `[buttons]`, and scroll lists with the
wheel. The tab labels show their shortcut, e.g. `Trace [t]`.

| Key | Function |
|-----|----------|
| `t` / `s` / `a` / `g` / `v` / `m` / `p` | Trace / Send / Actions / Guards / Graphs / Model / Playback tab |
| `←` `→` `↑` `↓` `Enter` | Navigate; `→` opens details (signals of an action, Lua output, guard log) |
| `Enter` in Trace | Into the signals of the selected frame; `Enter` on a signal toggles it in the current graph (or click its `□`) |
| `Space` | Pause/resume an action, enable/disable a guard, pause/resume the graph |
| `+` | Guards: add the guard's outputs to the current graph. Graphs: zoom in (`-` zooms out) |
| Graphs: `n` / `y` / `c` / `Del` | New graph / auto-Y / clear / remove the selected signal |
| `l` | Start a log (prompts for the file, default from `log_file_path`) / stop it |
| `o` | Open the current or last log in `log_viewer` (or the detected PlotJuggler) |
| `f` | Search in the trace |
| `q` / `Esc` | Quit |

The **Graphs** tab draws the selected signals as braille plots: the `graphs:` presets of the config,
plus everything added from Trace, Guards and Model. Guards and graphs also work with `--playback`.

The log format (MCAP, BLF or both) and its contents come from `logging:` in the config.

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
| `--config <file>` | Load the config from a YAML file instead of `caneo.yaml` (the GUI also saves there) |
