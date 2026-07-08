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
| `build/uwb_tag_firmware/zephyr/app_update.bin` | Signed update artifact for slot1 — what the BLE/SMP DFU transport (UWB-265, see "MCUboot & OTA" below) pushes via `img_mgmt`'s `image upload`. |
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
- **DFU transport — BLE/SMP/MCUmgr (UWB-265, this subissue, done):** see
  "BLE / SMP-MCUmgr DFU transport" below.
- **Not yet in scope** (later subissue per ADR-0040): image self-confirm /
  test-and-rollback / running-version reporting on the app side (UWB-266).
  Today an uploaded image can be tested (`image test`) and will boot, but
  nothing in `src/main.c` calls `boot_write_img_confirmed()` — without
  UWB-266, MCUboot's overwrite-only revert bookkeeping is the only safety
  net on a bad image. A CI-built signed update artifact and the full
  laptop-side DFU procedure are documented in UWB-267.

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

`src/main.c` only brings up the BLE stack and starts/re-arms advertising on
connect/disconnect — the `img_mgmt`/`os_mgmt` command handlers themselves are
registered automatically by `CONFIG_MCUMGR_GRP_IMG` / `CONFIG_MCUMGR_GRP_OS`
(no application code calls them). There is **no confirm/rollback logic
here** — see UWB-266.

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

### Hardware bring-up checklist (DWM1001, on-device — not verified by CI)

CI verifies the build produces the bootloader + a correctly signed app
image (see `.github/workflows/build.yml`). The following is **hardware-only**
and has **not** been exercised on a physical board as part of this change —
verify on a DWM1001 (or nrf52dk_nrf52832 J-Link target) before relying on it:

**MCUboot bring-up (UWB-264, carried forward):**

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

**BLE/SMP bring-up (UWB-265, new):**

6. After step 4's boot banner, confirm the log also shows `Bluetooth
   initialised` and `BLE advertising started as "UWB-Tag"` (RTT/UART, see
   "Viewing logs").
7. Scan for BLE devices from a phone/laptop (e.g. nRF Connect for
   Mobile/Desktop) and confirm **`UWB-Tag`** appears as a connectable
   advertiser.
8. Using the `mcumgr` CLI (`go install
   github.com/apache/mynewt-mcumgr-cli/mcumgr@latest`) or **nRF Connect
   Device Manager**, connect over BLE and run:
   ```bash
   mcumgr --conntype ble --connstring peer_name='UWB-Tag' os echo -l '{"echo":"hello"}'
   mcumgr --conntype ble --connstring peer_name='UWB-Tag' image list
   ```
   Expect the echoed string back from `os echo`, and `image list` to show
   `image=0 slot=0` as `active confirmed` with the version signed into
   `zephyr.signed.bin` (slot1 will be empty until an image has been
   uploaded).
9. Confirm the log shows `BLE central connected` on connect and `BLE central
   disconnected ... resuming advertising` after the client disconnects (i.e.
   the tag re-advertises and is reachable for a subsequent DFU attempt
   without a manual reset).
10. Optionally (smoke-test the upload path, without yet confirming/booting
    the new image — that full procedure is UWB-267): `mcumgr ... image
    upload build/uwb_tag_firmware/zephyr/zephyr.signed.bin` and confirm
    `image list` now shows the uploaded image in `image=0 slot=1`.

---

## Project layout

```
uwb_tag_firmware/
├── west.yml                    # west manifest — pins NCS v2.7.0
├── CMakeLists.txt              # Zephyr application cmake
├── prj.conf                    # Kconfig fragment (logging, UART, RTT, BLE + MCUmgr/SMP)
├── sysbuild.conf                # sysbuild config — enables MCUboot + swap mode + signing key
├── pm_static.yml                 # static flash partition layout (mcuboot/slot0/slot1/settings)
├── keys/
│   └── mcuboot_dev_key.pem      # MCUboot dev signing key (non-production, see header comment)
├── src/
│   └── main.c                 # boot banner + BLE/SMP-MCUmgr bring-up + idle loop
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
  UWB-264) — **done**, see "MCUboot & OTA" above.
- BLE peripheral + SMP/MCUmgr DFU transport (ADR-0040, UWB-265) — **done**,
  see "BLE / SMP-MCUmgr DFU transport" above. Still to come: image
  confirm/rollback/version-set on the app side (UWB-266) and the CI signed
  artifact + full laptop DFU procedure docs (UWB-267).
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
