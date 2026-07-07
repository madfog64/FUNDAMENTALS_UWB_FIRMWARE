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

### nRF52 DK (primary target, maps to DWM1001 via board overlay) — sysbuild + MCUboot

```bash
west build -b nrf52dk_nrf52832 . --sysbuild
```

`--sysbuild` is explicit above for clarity/CI determinism; on NCS >= v2.7,
sysbuild is the default `west build` flow, so a plain `west build -b
nrf52dk_nrf52832 .` also picks it up unless you've locally overridden
`west config build.sysbuild false`.

Sysbuild builds **two images** per `sysbuild.conf`
(`SB_CONFIG_BOOTLOADER_MCUBOOT=y`): the `mcuboot` bootloader and this
application, signed with the dev key at `keys/mcuboot_dev_key.pem`. See
`pm_static.yml` for the static 512 KB partition layout (mcuboot / slot0 /
slot1 / settings) and `sysbuild.conf` for the overwrite-only swap-mode
rationale.

Key outputs:

| Path | What it is |
|------|------------|
| `build/mcuboot/zephyr/zephyr.hex` | The bootloader image (flash once). |
| `build/uwb_tag_firmware/zephyr/zephyr.signed.hex` | Signed app image for slot0 — combined with mcuboot, this is what `west flash` programs. |
| `build/uwb_tag_firmware/zephyr/zephyr.signed.bin` | Same, raw binary. |
| `build/uwb_tag_firmware/zephyr/app_update.bin` | Signed update artifact for slot1 — what a future DFU transport (BLE/SMP, UWB-265) would push. |
| `build/dfu_application.zip` | Multi-image DFU bundle (manifest + signed app image), produced automatically because MCUboot is enabled under sysbuild. |

The file `boards/nrf52dk_nrf52832.overlay` is picked up automatically and
configures SPI1 with the DWM1001's DW1000 pin mapping.

### native_sim (headless / CI, no hardware required)

```bash
west build -b native_sim . --no-sysbuild
./build/zephyr/zephyr.exe
```

`native_sim` runs the Zephyr image as a Linux process — there is no flash to
partition and no MCUboot support for this "board", so `--no-sysbuild` is
required here to bypass `sysbuild.conf` (which otherwise applies
`SB_CONFIG_BOOTLOADER_MCUBOOT=y` unconditionally). Useful for logging
verification and unit tests (`west twister`, which builds each `tests/`
suite as its own app directory and is unaffected by the root
`sysbuild.conf`) without hardware.

### Pristine rebuild

```bash
west build -b nrf52dk_nrf52832 . --sysbuild --pristine
```

---

## Flashing

```bash
west flash                       # uses J-Link by default on nrf52dk
```

Under sysbuild, `west flash` programs **both** images — `mcuboot` and the
signed application (`zephyr.signed.hex`) — at their `pm_static.yml`
addresses in a single invocation.

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

## MCUboot & OTA

MCUboot is enabled via NCS **sysbuild** (ADR-0040). See `sysbuild.conf` for
the bootloader/swap-mode config and `pm_static.yml` for the static 512 KB
partition table (both files carry the full rationale in their header
comments). Summary:

- **Bootloader:** `mcuboot` image, 32 KiB, built from the NCS-pinned
  `bootloader/mcuboot` tree (via `west.yml`'s `import: true`).
- **Partitions (static, `pm_static.yml`):** `mcuboot` (32 KiB) /
  `mcuboot_primary` a.k.a. slot0 (232 KiB) / `mcuboot_secondary` a.k.a. slot1
  (232 KiB) / `settings_storage` (16 KiB) — exact fit in 512 KB, no slack.
- **Swap mode:** overwrite-only (no scratch partition) — see
  `sysbuild.conf` for why this was chosen over swap-with-test on this
  flash budget.
- **Signing:** dev key at `keys/mcuboot_dev_key.pem` (ECDSA P-256,
  **non-production**, see that file's header comment for the rotation
  checklist before any field deployment).
- **Not yet in scope** (later subissues per ADR-0040): the BLE/SMP-MCUmgr
  DFU transport that actually delivers an update image to slot1 (UWB-265),
  and image self-confirm / test-and-rollback / version reporting
  (UWB-266). Today, `west flash` writes directly into slot0 via the debug
  probe — there is no update path yet, only a bootable, signed image.

### Hardware bring-up checklist (DWM1001, on-device — not verified by CI)

CI verifies the build produces the bootloader + a correctly signed app
image (see `.github/workflows/build.yml`). The following is **hardware-only**
and has **not** been exercised on a physical board as part of this change —
verify on a DWM1001 (or nrf52dk_nrf52832 J-Link target) before relying on it:

1. `west build -b nrf52dk_nrf52832 . --sysbuild --pristine`
2. `west flash` — programs both `mcuboot` and the signed app.
3. Open RTT or UART (see "Viewing logs" above) *before* power-cycling/reset.
4. Confirm the **MCUboot banner** appears first (e.g. `*** Booting MCUboot ***`
   / `Starting bootloader` with the MCUboot build info), followed by the
   application's own `UWB Tag Firmware starting — board: ...` boot banner
   from `src/main.c` — i.e. the app is observed booting **through** MCUboot,
   not flashed standalone at address 0.
5. Confirm `west flash` did not report any partition-overflow / "image too
   large for slot" error from imgtool during signing (would show up during
   the `west build` step, not at flash time — flagging here as a first
   sanity check while on the bench).

---

## Project layout

```
uwb_tag_firmware/
├── west.yml                    # west manifest — pins NCS v2.7.0
├── CMakeLists.txt              # Zephyr application cmake
├── prj.conf                    # Kconfig fragment (logging, UART, RTT)
├── sysbuild.conf                # sysbuild config — enables MCUboot + swap mode + signing key
├── pm_static.yml                 # static flash partition layout (mcuboot/slot0/slot1/settings)
├── keys/
│   └── mcuboot_dev_key.pem      # MCUboot dev signing key (non-production, see header comment)
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
- MCUboot bootloader + static partitions + dev signing key (ADR-0040,
  UWB-264) — **done**, see "MCUboot & OTA" above. Still to come: BLE/SMP
  MCUmgr DFU transport (UWB-265), image confirm/rollback/version (UWB-266).
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
