# Raspberry Pi 5 + PCAN-M.2: caneo gateway image (Yocto)

Yocto **scarthgap** (5.0 LTS, GCC 13, glibc 2.39) image for a Raspberry Pi 5 with a
**PEAK PCAN-M.2** (4 channels, CAN FD) on an M.2 HAT. caneo is in the image and
starts at boot as **headless gRPC server**. The PC connects to it, receives all
frames and can send:

On the PC: Settings > General > **Remote (gRPC)** on, then in Settings > Channels add the block
**gRPC SocketCAN** (address `192.168.10.75:50051`). It asks the Pi for its channels (can0..can3); tick the
ones you need, connect them to Decode / Live / Log like local channels, Save & Restart. A prefix (e.g. `pi_`)
avoids name clashes with local channels. **Reset HW** in the menu bar (while the channels run) restarts the
CAN controllers on the Pi (down + up, e.g. after bus-off); the service may do that thanks to `CAP_NET_ADMIN`. Alternatively all channels via the command line:

```bash
caneo --grpc_client=192.168.10.75:50051      # on the PC
```

| What            | Value                                                          |
|-----------------|----------------------------------------------------------------|
| Network         | `eth0` static `192.168.10.75/24`, no gateway (systemd-networkd) |
| Login           | user `can`, password `can` (SSH, sudo)                          |
| can0, can1, can3| CAN 1 Mbit/s                                                    |
| can2            | CAN FD 1 Mbit/s / 5 Mbit/s data                                 |
| caneo           | `caneo.service`: `caneo --headless --grpc_server --grpc_port=50051 --config /etc/caneo/caneo.yaml` |

## Build

Needs only Docker (the build runs in Ubuntu 22.04, `docker/Dockerfile`).
About 100 GB of disk space; the first run takes a few hours.

```bash
./build.sh            # = sdk + caneo + image
./build.sh sdk        # bitbake meta-toolchain, installs the SDK to sdk/install
./build.sh caneo      # cross-builds caneo with the SDK + conan (headless, CANEO_GUI=OFF)
./build.sh image      # bitbake caneo-image -> deploy/
./build.sh shell      # bitbake shell in the container
```

After changing caneo: `./build.sh caneo && ./build.sh image`. Alternatively, copy only the binary:
`scp sdk/build-caneo/caneo can@192.168.10.75:/tmp/ && ssh can@192.168.10.75 'sudo install /tmp/caneo /usr/bin/caneo && sudo systemctl restart caneo'`.

### How caneo gets into the image

1. `meta-toolchain` gives a standard Yocto SDK (cross gcc 13 + sysroot of the image's glibc).
2. `build.sh caneo` creates a conan profile `rpi5` from the SDK's `environment-setup-*` script
   (compiler, `-mcpu=cortex-a76…`, `--sysroot`) and builds all conan dependencies + caneo
   for aarch64. Build-context tools (protoc, grpc_cpp_plugin) are built natively.
   Conan cache: `sdk/home/.conan2`.
3. The binary is copied to `meta-caneo/recipes-apps/caneo/files/caneo`. The `caneo` recipe installs it.

## Flashing

```bash
sudo bmaptool copy deploy/caneo-image-raspberrypi5.rootfs.wic.bz2 /dev/sdX
# or: bzcat deploy/caneo-image-raspberrypi5.rootfs.wic.bz2 | sudo dd of=/dev/sdX bs=4M status=progress conv=fsync
```

## Layer `meta-caneo`

| File | Purpose |
|------|---------|
| `recipes-kernel/linux/files/can-peak.cfg` | SocketCAN + `CONFIG_CAN_PEAK_PCIEFD` (driver of the PCAN-M.2) + vcan |
| `conf/local.conf` (`RPI_EXTRA_CONFIG`) | `dtparam=pciex1` in `config.txt` enables the external PCIe connector; `dtoverlay=pcie-32bit-dma-pi5` is **required**: the PCAN FPGA only does 32-bit DMA, without it `peak_pciefd` fails with `Rx dmam_alloc_coherent(4096) failure` / `-12` and there are no canX |
| `recipes-core/caneo-system` | `10-eth.network` (static IP), `80-canN.network` (bitrates, FD), sudo for `can` |
| `recipes-apps/caneo` | binary, `/etc/caneo/caneo.yaml`, `caneo.service`, `caneo-wait-can`, user `can` |
| `recipes-core/images/caneo-image.bb` | image: boot, SSH, kernel modules, can-utils, caneo; password of `can` |

The CAN channels are configured by **systemd-networkd** at boot (`/etc/systemd/network/80-can*.network`).
When caneo starts, the channels are already up, so caneo does not need root. To change a bitrate,
edit the `.network` file **and** `/etc/caneo/caneo.yaml`, then
`sudo systemctl restart systemd-networkd caneo`.

## Checks on the Pi

```bash
lspci -nn | grep -i 001c          # PEAK (vendor 0x001c)
ip -details link show can2        # bitrate 1000000, dbitrate 5000000, mtu 72
systemctl status caneo
journalctl -u caneo -f
candump any
```
