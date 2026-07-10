# UWB Tag Firmware

Tag firmware for the DWM1001 module (Nordic nRF52832 + Decawave DW1000).
Built on **nRF Connect SDK (NCS) v2.7.0** / Zephyr RTOS.

See `docs/adr/0008-firmware-toolchain.md` in the `FUNDAMENTALS_SPORTS` repo for the
toolchain decision rationale.

For flashing a tag and updating it over BLE (initial SWD flash, the
`mcumgr`/nRF Connect Device Manager DFU sequence, the on-hardware bring-up
checklist, and the dev-key/non-production caveat), see
**[`docs/ota.md`](docs/ota.md)**.

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
| `build/uwb_tag_firmware/zephyr/zephyr.signed.bin` | Same, raw binary — **also the file uploaded over the BLE/SMP DFU transport** (UWB-265, see "MCUboot & OTA" below) via `img_mgmt`'s `image upload`; MCUmgr computes chunk offsets itself, so no separate "update" binary is needed (verified against this repo's sysbuild output, UWB-267 — earlier drafts of this doc referenced a nonexistent `app_update.bin`; that filename comes from NCS's older, non-sysbuild child-image build flow and this repo's sysbuild config does not produce it). |
| `build/dfu_application.zip` | Multi-image DFU bundle (manifest + signed app image, named `<image>.bin` inside — e.g. `uwb_tag_firmware.bin`), produced automatically because MCUboot is enabled under sysbuild. This is the file nRF Connect Device Manager expects for a manifest-driven upload; `mcumgr` CLI uploads the plain `zephyr.signed.bin` instead (see `docs/ota.md`). |

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
- **DFU transport — BLE/SMP/MCUmgr (UWB-265, done):** see "BLE / SMP-MCUmgr
  DFU transport" below.
- **Image self-confirm, health gate & version reporting (UWB-266, done):**
  see "Image self-confirm, health gate & version reporting" below.
- **CI signed artifact + laptop DFU procedure (UWB-267, done):** CI already
  builds via sysbuild and uploads `zephyr.signed.hex`/`.bin` and
  `dfu_application.zip` as the `uwb-tag-firmware-signed` artifact on every
  run (see `.github/workflows/build.yml`). The full flash + BLE-DFU
  procedure (`mcumgr` CLI and nRF Connect Device Manager, plus the
  consolidated on-hardware bring-up checklist and the dev-key caveat) is
  **[`docs/ota.md`](docs/ota.md)**.

### BLE / SMP-MCUmgr DFU transport (UWB-265)

The tag runs as a **BLE peripheral** advertising as a connectable device
named **`UWB-Tag`** (`CONFIG_BT_DEVICE_NAME`, `prj.conf`) and exposing the
**SMP GATT characteristic** (Zephyr's `CONFIG_MCUMGR_TRANSPORT_BT`). Any SMP
client that connects (the `mcumgr` Go CLI, or **nRF Connect Device Manager**
on desktop/mobile) can drive:

- **`img_mgmt`** — `image list` / `image upload` / `image test` / `image
  confirm`. `image upload` writes into the MCUboot **secondary slot**
  (`mcuboot_secondary`, a.k.a. slot1, `0x42000`–`0x7C000` per
  `pm_static.yml`) — it never touches the running slot0 image directly.
- **`os_mgmt`** — `echo`, `reset`, `taskstat`. The running image's version
  is read from the MCUboot image header via `image list`, not a separate
  `os_mgmt` field.

`src/main.c` brings up the BLE stack and starts/re-arms advertising on
connect/disconnect, and runs the UWB-266 self-check/confirm gate — the
`img_mgmt`/`os_mgmt` command handlers themselves are registered
automatically by `CONFIG_MCUMGR_GRP_IMG` / `CONFIG_MCUMGR_GRP_OS` (no
application code calls them).

The link is currently **unauthenticated** (`CONFIG_MCUMGR_TRANSPORT_BT_AUTHEN=n`)
— acceptable for a bench-stage build (ADR-0040 defers secure-boot/production
hardening, including DFU-link auth, to a later ADR); do not treat this as a
production security posture.

**BLE ↔ UWB coexistence note:** the nRF52832's BLE radio and the DW1000
(SPI-attached UWB transceiver) are RF-independent — enabling BLE does not
contend for airtime with UWB ranging/blink frames. The risk is **CPU/timing
contention**, not RF: BLE connection events and the MCUmgr command handlers
run on the system work queue, which can perturb the tight TDMA slot timing
(ADR-001/ADR-003) if a firmware upload happens *during* an active tracking
session. Per ADR-0040, the accepted baseline is that **OTA runs in a
maintenance/idle window, not mid-session** — this is documented, not
enforced in code; there is no mutual-exclusion mechanism between "tag is
blinking in its TDMA slot" and "tag is accepting a BLE image upload" as of
this subissue. Solving that (e.g. refusing/pausing uploads while joined to a
tracking cycle) is a follow-on concern, not a UWB-265 requirement.

### Image self-confirm, health gate & version reporting (UWB-266)

`src/main.c` calls `confirm_image_if_healthy()` early in `main()` (before BLE
bring-up). It skips the check entirely if `boot_is_img_confirmed()` already
reports confirmed (true on a fresh `west flash` — MCUboot treats a
never-swapped, "preprogrammed" image as confirmed by definition — and true
again on every subsequent warm boot of an already-confirmed image). Otherwise
it runs the boot self-check gate and, only on success, calls
`boot_write_img_confirmed()` (`<zephyr/dfu/mcuboot.h>`) to mark the image
permanent.

- **The gate (`src/image_health.h` / `.c` / `_zephyr.c`):** deliberately
  small and explicit per UWB-266 scope — "core subsystems initialised", not
  full UWB/DW1000 bring-up. It checks the kernel is running, logging is up,
  and the DW1000 SPI bus + reset/IRQ GPIO controllers report
  `device_is_ready()`/`gpio_is_ready_dt()` (a wiring/presence check only —
  no SPI transaction to the DW1000 chip itself, no `dwt_initialise()`, no
  register read). A `TODO` in both files marks where an actual "DW1000
  responds" check hooks in once `dw1000_port_init()`/`dw1000_configure()`
  are wired into `src/main.c` (UWB-91/UWB-92 follow-on) — not done here per
  the ticket's explicit "do NOT block on full UWB bring-up."
  - The decision logic (`image_health_evaluate()`, `image_health.c`) is
    pure — no Zephyr includes — and is unit-tested in `tests/image_health`
    (ztest/`unit_testing`, host-run). The check-gathering side
    (`image_health_run_checks()`, `image_health_zephyr.c`) is
    Zephyr/devicetree-only and is not part of that host test; it is
    exercised by the real board build and the on-device checklist below.
- **On failure:** the gate logs an error and leaves the image unconfirmed —
  it deliberately does **not** force a reboot. See the honesty note below on
  why a reboot would not help and could actively hurt (bootloop with no
  recovery path).
- **Version (single source):** the top-level `VERSION` file is now the one
  source of the app's image version. Zephyr auto-generates
  `app_version.h` from it (`APP_VERSION_STRING` etc., see
  `zephyr/doc/build/version/index.rst`), which `src/main.c` logs in the boot
  banner (`UWB Tag Firmware starting — board: ..., version: ..., built:
  ...`). `CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` (`zephyr/modules/Kconfig.mcuboot`)
  defaults to the same `VERSION` file whenever it exists (no `prj.conf`
  override added here, on purpose — a second, hand-set value would be a
  second source of truth and could drift from the file). That is also the
  version imgtool signs into the MCUboot image header, which is what
  `mcumgr image list` reports over SMP (`os_mgmt`/`img_mgmt`, UWB-265) — no
  extra application code is needed for that half, it falls out of signing
  from the same `VERSION` file.

**Honesty note — what "test-and-rollback" actually means here:** MCUboot is
configured `SB_CONFIG_MCUBOOT_MODE_OVERWRITE_ONLY=y` (UWB-264,
`sysbuild.conf`). In this mode MCUboot has **no test/revert state machine at
all** — verified against this repo's pinned NCS v2.7.0 MCUboot source
(`bootloader/mcuboot/boot/bootutil/src/loader.c`,
`.../bootutil_misc.c`): `boot_perform_update()` unconditionally erases the
primary slot and overwrites it with the secondary slot's image
(`boot_copy_image()`), and the `image_ok`/`copy_done` trailer bookkeeping
that swap-with-test modes rely on for "revert unless confirmed" is compiled
out entirely under `#ifndef MCUBOOT_OVERWRITE_ONLY`. Once an image has been
overwritten into the primary slot, there is no backup left anywhere to
revert to. `sysbuild.conf`'s header comment previously claimed otherwise
("overwrite-only mode still reverts... via its own boot-count/confirm
bookkeeping") — that was incorrect and has been corrected in that file as
part of UWB-266.

Practical consequence: `boot_write_img_confirmed()` in this configuration
cannot make MCUboot roll back a bad-but-bootable image — there is nothing to
roll back to. What UWB-266 actually delivers is:
  - A real (if narrow) health gate that only confirms a genuinely-booted,
    minimally-sane image — a truly broken image (crash/hang before reaching
    the gate) never confirms, which is visible indefinitely over `mcumgr
    image list` as `confirmed: false`.
  - No harmful "fix" attempt on failure: since there is no previous image to
    fall back to, forcing a reboot on gate failure would only bootloop the
    same bad image with no path to recovery. The only real recovery for a
    bad-but-bootable image is uploading a fresh known-good image over
    BLE/SMP (UWB-265) — which stays reachable precisely because the gate
    does not reboot.

### Hardware bring-up checklist (DWM1001, on-device — not verified by CI)

CI verifies the build produces the bootloader + a correctly signed app
image (see `.github/workflows/build.yml`). The full ordered checklist —
covering MCUboot bring-up (UWB-264), BLE/SMP bring-up (UWB-265), and the
self-confirm/health-gate/version behaviour (UWB-266) — is **hardware-only**
and has **not** been exercised on a physical board as part of this repo's
CI. It now lives in **[`docs/ota.md`](docs/ota.md) §3** (consolidated there
alongside the flash and BLE-DFU procedure, UWB-267) rather than duplicated
here — verify on a DWM1001 (or nrf52dk_nrf52832 J-Link target) before
relying on any of it.

---

## Project layout

```
uwb_tag_firmware/
├── west.yml                    # west manifest — pins NCS v2.7.0
├── CMakeLists.txt              # Zephyr application cmake
├── prj.conf                    # Kconfig fragment (logging, UART, RTT, BLE + MCUmgr/SMP)
├── sysbuild.conf                # sysbuild config — enables MCUboot + swap mode + signing key
├── pm_static.yml                 # static flash partition layout (mcuboot/slot0/slot1/settings)
├── VERSION                      # single source of the app image version (UWB-266)
├── keys/
│   └── mcuboot_dev_key.pem      # MCUboot dev signing key (non-production, see header comment)
├── src/
│   ├── main.c                  # boot banner + self-check/confirm gate + BLE/SMP-MCUmgr bring-up
│   ├── image_health.h          # health-gate types + evaluate()/run_checks() API (UWB-266)
│   ├── image_health.c          # pure gate decision logic (host-testable, UWB-266)
│   └── image_health_zephyr.c   # Zephyr-side check gathering (UWB-266)
├── tests/
│   └── image_health/           # ztest host suite for image_health_evaluate() (UWB-266)
├── boards/
│   └── nrf52dk_nrf52832.overlay  # DWM1001 SPI1/GPIO pin mapping for DW1000
├── docs/
│   └── ota.md                   # flash + BLE-DFU procedure, bring-up checklist (UWB-267)
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
  UWB-264) — **done**, see "MCUboot & OTA" above.
- BLE peripheral + SMP/MCUmgr DFU transport (ADR-0040, UWB-265) — **done**,
  see "BLE / SMP-MCUmgr DFU transport" above.
- Image self-confirm, health gate & version reporting (ADR-0040, UWB-266) —
  **done**, see "Image self-confirm, health gate & version reporting" above.
- CI signed-update artifact + host DFU/bring-up procedure docs (ADR-0040,
  UWB-267) — **done**, see "MCUboot & OTA" above and `docs/ota.md`. This
  closes out the ADR-0040 OTA epic.
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
