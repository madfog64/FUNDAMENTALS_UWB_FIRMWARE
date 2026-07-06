# UWB Tag Firmware

Tag firmware for the DWM1001 module (Nordic nRF52832 + Decawave DW1000).
Built on **nRF Connect SDK (NCS) v2.7.0** / Zephyr RTOS.

See `docs/adr/0008-firmware-toolchain.md` in the `FUNDAMENTALS_SPORTS` repo for the
toolchain decision rationale.

---

## Pinned NCS version

**NCS v2.7.0** — tracked via `west.yml` in this repo.  `west update` fetches
the exact revisions of Zephyr, mcuboot, and all NCS modules pinned by that
release.  Never edit `west.yml` to use `main`; always pin a release tag.

---

## Prerequisites

Install the nRF Connect SDK toolchain exactly once per machine.  The
recommended method is the **nRF Connect for VS Code** extension (bundles
`west`, the Zephyr SDK ARM toolchain, and nRF Util) or manual installation
following Nordic's docs:

  https://developer.nordicsemi.com/nRF_Connect_SDK/doc/2.7.0/nrf/installation.html

Key tools needed on `PATH`:

| Tool | Version |
|------|---------|
| `west` | >= 1.2 |
| `cmake` | >= 3.20 |
| `python3` | >= 3.10 |
| `arm-zephyr-eabi-gcc` | Zephyr SDK 0.16.x (bundled with NCS toolchain) |

---

## Workspace initialisation (one-time)

A west workspace has this layout after setup:

```
<workspace>/
├── .west/
├── uwb_tag_firmware/    ← this repo
├── nrf/
├── zephyr/
├── modules/
└── ...
```

### Option A — clone then init (recommended for CI)

```bash
mkdir uwb_workspace && cd uwb_workspace
git clone https://github.com/madfog64/FUNDAMENTALS_UWB_FIRMWARE uwb_tag_firmware
west init -l uwb_tag_firmware   # reads uwb_tag_firmware/west.yml
west update                     # clones NCS v2.7.0, Zephyr, modules (~5 GB)
```

### Option B — init from NCS then overlay this app

```bash
west init --mr v2.7.0 uwb_workspace
cd uwb_workspace
west update
# clone this repo into the workspace
git clone https://github.com/madfog64/FUNDAMENTALS_UWB_FIRMWARE uwb_tag_firmware
```

> **Tip:** set `ZEPHYR_BASE` to `<workspace>/zephyr` in your shell profile so
> that `west build` outside the workspace still works:
> ```bash
> export ZEPHYR_BASE=~/uwb_workspace/zephyr
> ```

---

## Building

All commands assume you are in the `uwb_tag_firmware/` directory (or that
`ZEPHYR_BASE` is exported).

### nRF52 DK (primary target, maps to DWM1001 via board overlay)

```bash
west build -b nrf52dk_nrf52832 .
```

Produces `build/zephyr/zephyr.hex` and `build/zephyr/zephyr.elf`.

The file `boards/nrf52dk_nrf52832.overlay` is picked up automatically and
configures SPI1 with the DWM1001's DW1000 pin mapping.

### native_sim (headless / CI, no hardware required)

```bash
west build -b native_sim .
./build/zephyr/zephyr.exe
```

`native_sim` runs the Zephyr image as a Linux process.  The boot banner is
emitted to stdout via the UART console backend.  Useful for logging verification
and unit tests (`west twister`) without hardware.

### Pristine rebuild

```bash
west build -b nrf52dk_nrf52832 . --pristine
```

---

## Flashing

```bash
west flash                       # uses J-Link by default on nrf52dk
```

### Viewing logs

**RTT (J-Link RTT Viewer):**
```bash
JLinkRTTViewerExe                # GUI, select "RTT channel 0"
# or
west debug --runner jlink        # opens GDB + RTT in background
```

**UART (USB CDC on nrf52dk):**
```bash
screen /dev/ttyACM0 115200
# or
minicom -D /dev/ttyACM0 -b 115200
```

---

## Project layout

```
uwb_tag_firmware/
├── west.yml                    # west manifest — pins NCS v2.7.0
├── CMakeLists.txt              # Zephyr application cmake
├── prj.conf                    # Kconfig fragment (logging, UART, RTT)
├── src/
│   └── main.c                 # boot banner + idle loop
├── boards/
│   └── nrf52dk_nrf52832.overlay  # DWM1001 SPI1/GPIO pin mapping for DW1000
└── README.md                   # this file
```

Upcoming additions (later subissues):
- `drivers/dw1000/` — Zephyr SPI port of `deca_driver` (UWB-91)
- `drivers/dw1000/dw1000_sync.{c,h}` — tag-side SYNC-frame RX + validate/parse
  (UWB-242); maintaining the tag cycle reference and the tracking-loop seam
  is sibling UWB-243, not yet implemented
- `drivers/dw1000/twr_responder.{c,h}` — DS-TWR responder mode state machine
  (UWB-232, Phase-1 calibration, ADR-001) + `samples/twr_responder/`;
  two-mode (responder<->blinker) switch + registration/join is UWB-9/10/11,
  not yet implemented
- `src/uwb_blink.c` — TDoA blink application (UWB-92)
- `boards/dwm1001.conf` / full custom board definition (if needed)
- MCUboot OTA integration (ADR-008, UWB-94)
- `.github/workflows/` — CI build + twister (UWB-91 or dedicated ticket)

---

## Development notes

- **Never build on the `main` branch.**  Branch off `main`, build, open a PR.
- The `build/` directory is git-ignored; do not commit build artefacts.
- Kconfig fragments for a specific board go in a file named
  `boards/<board>.conf` alongside the overlay.
- Reference firmware (nRF5 SDK, read-only): `~/development/UWB_FIRMWARE`.
  Mine `deca_driver/` for DW1000 init sequences and timing constants; do not
  carry over nRF5-SDK scaffolding.
