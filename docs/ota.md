# Tag OTA — flash + DFU procedure

Operator-facing procedure for getting firmware onto a DWM1001/nRF52dk tag and
updating it in the field over BLE. This is the "how do I actually do it" doc;
the "why this design" writeup is `docs/adr/0040-tag-ota-transport.md` in the
`FUNDAMENTALS_SPORTS` repo, and the implementation notes for each piece live
in `README.md`'s "MCUboot & OTA" section (bootloader/partition layout, the
BLE/SMP transport, and the self-confirm/health-gate/version logic). This doc
consolidates the on-hardware bring-up steps carried across UWB-264/265/266
and closes UWB-267.

**Scope note:** everything below is the **tag** (DWM1001/nRF52832) update
path only — MCUboot + SMP-over-BLE from a laptop/phone. Anchor OTA (IoT
Jobs) and an anchor-BLE-relay to reach tags remotely are explicitly out of
scope (ADR-0040) and not covered here.

---

## 0. Dev-key / non-production caveat — read this first

The signed images below (both the ones `west build` produces locally and the
ones CI publishes as artifacts) are signed with the **development key**
committed at `keys/mcuboot_dev_key.pem` (ECDSA P-256). That key's own header
comment has the full rationale and rotation checklist; the short version:

- It exists so every developer and CI runner produces a **reproducible**
  signed image without an out-of-band key-distribution step.
- MCUboot embeds the corresponding **public** key into the bootloader at
  build time and will refuse to boot an application image signed with a
  different key — so as long as a device's bootloader and the image you
  upload were built from the same `keys/mcuboot_dev_key.pem`, DFU works.
- **Do not use this key for a production/field deployment.** Secure-boot
  hardening — a real key in restricted custody (HSM/secrets manager, not
  git), `CONFIG_MCUBOOT_HW_KEY`/KMU, image encryption, anti-rollback
  counters — is deliberately deferred (ADR-0040 "Signing — dev baseline now,
  hardened later") and tracked as follow-on work, not done here. Treat every
  procedure below as **bench/lab-only**.
- The BLE/SMP link itself is also **unauthenticated**
  (`CONFIG_MCUMGR_TRANSPORT_BT_AUTHEN=n`) — anyone in BLE range who knows the
  device is advertising as `UWB-Tag` can connect and drive `img_mgmt`/
  `os_mgmt`. Acceptable for a bench build; not a production security
  posture.

---

## 1. Initial flash (SWD, J-Link)

This is the only step that requires a debugger — every subsequent update
goes over BLE (§2).

```bash
# From the west workspace (see README.md "Workspace initialisation"),
# with uwb_tag_firmware/ as the app directory:
west build -b nrf52dk_nrf52832 uwb_tag_firmware --sysbuild --pristine
west flash
```

Under sysbuild this programs **both** images in one invocation — the
`mcuboot` bootloader (`build/mcuboot/zephyr/zephyr.hex`) and the signed
application for slot0 (`build/uwb_tag_firmware/zephyr/zephyr.signed.hex`) —
at the addresses fixed in `pm_static.yml`. See README.md "Building" /
"Flashing" for target details (nrf52dk_nrf52832 is the primary target,
mapping to the DWM1001 pin layout via `boards/nrf52dk_nrf52832.overlay`) and
"Viewing logs" for RTT/UART setup.

**Verify the flash worked** (RTT or UART, opened *before* the reset so you
don't miss the banner):

1. The **MCUboot banner** appears first (e.g. `*** Booting MCUboot ***`),
   followed by the application's own boot banner from `src/main.c`:
   ```
   UWB Tag Firmware starting — board: nrf52dk_nrf52832, version: 0.1.0, built: ...
   ```
   i.e. the app is booting **through** MCUboot, not flashed standalone.
2. `west flash` did not report a partition-overflow / "image too large for
   slot" error from imgtool (that would surface during `west build`'s
   signing step, not at flash time).
3. `Bluetooth initialised` and `BLE advertising started as "UWB-Tag"` appear
   shortly after — confirms the device is now reachable for BLE DFU (§2).

A fresh `west flash` image is **already confirmed** by MCUboot (a
never-swapped, preprogrammed image counts as confirmed by definition) — you
will not see a self-check log line on this first boot, only `Image already
confirmed — skipping self-check` at `DBG` level. That's expected; the
self-check gate (UWB-266) only runs after a real BLE-DFU swap (§2).

---

## 2. BLE DFU update (SMP/MCUmgr over BLE)

The tag advertises as **`UWB-Tag`** (`CONFIG_BT_DEVICE_NAME`, `prj.conf`) — a
connectable BLE peripheral exposing the MCUmgr SMP GATT characteristic
(`CONFIG_MCUMGR_TRANSPORT_BT`). Any SMP client can drive the update; two are
documented below. Both follow the same underlying sequence:

**upload → test → reset → (auto-confirm on a healthy boot, UWB-266) → verify**

Unlike NCS's older, non-sysbuild "child image" build flow (which produces a
separate `app_update.bin`), this repo's build is plain **Zephyr sysbuild**,
which signs a single `zephyr.signed.bin`/`.hex` pair and lets MCUmgr itself
compute upload chunk offsets — there is no separate "update-only" binary to
generate or look for (verified against a local sysbuild build, UWB-267). The
same signed binary is used for both the initial SWD flash (§1) and the BLE
upload below; only the **destination slot differs** (SWD flash writes slot0
directly, `mcumgr`/nRF Connect Device Manager `image upload` always writes
into MCUboot's secondary slot, slot1 — it never touches the running slot0
image directly, regardless of which file you pushed):

| File | Produced by | Used for |
|------|-------------|----------|
| `build/uwb_tag_firmware/zephyr/zephyr.signed.bin` | local `west build --sysbuild`, or downloaded from the CI artifact `uwb-tag-firmware-signed` (see §4) | `mcumgr image upload` (§2a) — the canonical Zephyr/NCS mcumgr upload target |
| `build/dfu_application.zip` | same | manifest-wrapped equivalent (contains `uwb_tag_firmware.bin` + `manifest.json`) — what nRF Connect Device Manager expects for a DFU package upload (§2b) |
| `build/uwb_tag_firmware/zephyr/zephyr.signed.hex` | same | SWD flashing only (§1) — **not** what you upload over BLE (`mcumgr`/Device Manager both take a `.bin`, not a `.hex`) |

### 2a. Using the `mcumgr` CLI

Install once: `go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest`.

```bash
# 1. Upload the new image into MCUboot's secondary slot (slot1) over BLE.
#    Takes a while over BLE's throughput — progress prints as it goes.
mcumgr --conntype ble --connstring peer_name='UWB-Tag' \
  image upload build/uwb_tag_firmware/zephyr/zephyr.signed.bin

# 2. Confirm the upload landed and grab its hash for the next step.
mcumgr --conntype ble --connstring peer_name='UWB-Tag' image list
#   image=0 slot=0  ... active confirmed  ver: 0.1.0
#   image=0 slot=1  ... hash:<HASH>       ver: 0.1.1   <- your uploaded image

# 3. Mark the uploaded image "test" (pending, one-time boot on next reset —
#    this is what lets the UWB-266 self-check gate run and decide whether to
#    confirm it).
mcumgr --conntype ble --connstring peer_name='UWB-Tag' \
  image test <HASH>

# 4. Reset the device to boot into the new image.
mcumgr --conntype ble --connstring peer_name='UWB-Tag' reset
```

After the reset, watch RTT/UART: a healthy boot logs `Boot self-check
passed — image confirmed permanent` (the UWB-266 gate calling
`boot_write_img_confirmed()`). Re-run `image list` — the new image should
now show `active confirmed` with the bumped `ver:` field.

```bash
# 5. Read the running version at any time (also works before/without a DFU):
mcumgr --conntype ble --connstring peer_name='UWB-Tag' image list
```

The version string comes from the top-level `VERSION` file at build time —
`CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` signs it into the MCUboot image
header, and that header field is exactly what `image list`'s `ver:` reports
(no separate `os_mgmt` version field or application code involved — see
README.md "Image self-confirm, health gate & version reporting").

### 2b. Using nRF Connect Device Manager (GUI, desktop or mobile)

Same four steps, no CLI:

1. Open **nRF Connect Device Manager**, scan, and connect to **`UWB-Tag`**.
2. Go to the **Image** / **DFU** tab, pick **Upload**, and select
   `dfu_application.zip` (the manifest-wrapped package; `zephyr.signed.bin`
   also works if your client accepts a plain binary) from the build output
   or the downloaded CI artifact. Wait for the upload to complete.
3. In the resulting image list, select the newly-uploaded slot1 image and
   choose **Test** (not "Confirm" — confirm-without-test would skip the
   UWB-266 self-check's chance to run before the swap, and this repo's
   overwrite-only MCUboot mode has no way to undo a bad confirm-without-test
   swap; see the honesty note in README.md).
4. Trigger **Reset**. The app reconnects automatically once the tag
   re-advertises after boot.
5. Re-open the **Image** tab (or its **Echo**/`os_mgmt` equivalent) and
   confirm the active image is now the new version and shows **confirmed**
   — this readback is the GUI equivalent of `mcumgr image list`.

### 2c. What "auto self-confirm" means here (UWB-266)

You do **not** need a separate manual "confirm" step after `test` + `reset`
in normal operation. `src/main.c` calls `confirm_image_if_healthy()` early
in `main()`, before BLE bring-up, on every boot of a not-yet-confirmed
image:

- **Pass** (core subsystems up — kernel running, logging up, DW1000 SPI
  bus + reset/IRQ GPIO controllers report ready — see
  `src/image_health.h`/`.c` for the exact, deliberately narrow checks):
  logs `Boot self-check passed — image confirmed permanent` and calls
  `boot_write_img_confirmed()`. No mcumgr/GUI action needed.
- **Fail:** logs `Boot self-check FAILED — image left unconfirmed` and does
  **not** reboot. `image list` will show that image `active` but **not**
  `confirmed`, indefinitely.

**Important limitation — read before relying on "test-and-rollback":** this
repo's MCUboot is configured **overwrite-only**
(`SB_CONFIG_MCUBOOT_MODE_OVERWRITE_ONLY=y`), which has **no revert
mechanism at all** — once `test` + `reset` swaps a new image into the
primary slot, the previous image is gone; there is nothing left to roll
back to. A failed self-check therefore does **not** get you your old image
back automatically. The only recovery from a bad-but-bootable image is
uploading a corrected image over BLE/SMP and repeating §2a/§2b — which stays
reachable specifically because the gate does not force a reboot loop. See
README.md's "Honesty note — what 'test-and-rollback' actually means here"
for the full writeup (including the `bootutil`/`loader.c` source citation).

---

## 3. On-hardware bring-up checklist

Not exercised on a physical board as part of the CI/docs work in UWB-267 —
**verify on a real DWM1001 (or nrf52dk_nrf52832 J-Link target) before
relying on any of this.** This consolidates the hardware-only steps
deferred from UWB-264/265/266 into one ordered pass.

**MCUboot bring-up (UWB-264):**

1. `west build -b nrf52dk_nrf52832 uwb_tag_firmware --sysbuild --pristine`
2. `west flash` — programs both `mcuboot` and the signed app (§1).
3. Open RTT or UART *before* power-cycling/reset.
4. Confirm the MCUboot banner appears first, then the app's own boot banner
   (§1, item 1) — i.e. the app boots **through** MCUboot, not standalone.
5. Confirm no partition-overflow / "image too large for slot" error was
   reported by imgtool during the `west build` signing step.

**BLE/SMP bring-up (UWB-265):**

6. After step 4's banner, confirm `Bluetooth initialised` and `BLE
   advertising started as "UWB-Tag"` in the log.
7. Scan for BLE devices from a phone/laptop and confirm **`UWB-Tag`**
   appears as a connectable advertiser.
8. `mcumgr --conntype ble --connstring peer_name='UWB-Tag' os echo -l
   '{"echo":"hello"}'` echoes back, and `image list` shows `image=0 slot=0`
   as `active confirmed` with slot1 empty.
9. Confirm the log shows `BLE central connected` on connect and `BLE
   central disconnected ... resuming advertising` after disconnect — the
   tag re-advertises without a manual reset.
10. Smoke-test the upload path only (§2a step 1) and confirm `image list`
    shows the uploaded image in `image=0 slot=1`.

**Self-confirm, health gate & version (UWB-266):**

11. Confirm the boot banner's `version:` field matches the `VERSION` file
    (e.g. `version: 0.1.0`), matching `mcumgr image list`'s `ver:` for
    `image=0 slot=0`.
12. On the *first* boot after `west flash` (no upgrade yet), confirm the log
    shows only `Image already confirmed — skipping self-check` at `DBG`
    level — not a self-check pass/fail line. Expected: a never-swapped image
    is confirmed by definition.
13. **Exercise the gate on a real upgrade cycle:** bump `VERSION`
    (e.g. `VERSION_TWEAK`), build, run the full §2a/§2b sequence
    (upload → test → reset). Confirm the log shows `Boot self-check passed
    — image confirmed permanent` and `image list` reports the new image
    `active confirmed`.
14. **Confirm persistence across a power cycle** (not just `reset`): with
    step 13's image confirmed, fully power-cycle the board and confirm it
    boots the same image, still `confirmed`.
15. **Deliberately-failing image (and the honest limitation):** temporarily
    force `image_health_evaluate()` (`src/image_health.c`) to always return
    `IMAGE_HEALTH_FAIL`, build, upload + test it, reset. Confirm the log
    shows `Boot self-check FAILED — image left unconfirmed` and `image
    list` reports it `active` but not `confirmed`, indefinitely — **do not
    expect a revert to the previous image** (§2c). Revert this temporary
    change before flashing/uploading a real image.

---

## 4. Grabbing a signed image from CI (no local NCS toolchain needed)

Every push/PR build (`.github/workflows/build.yml`, `west-build` job)
already runs `west build -b nrf52dk_nrf52832 uwb_tag_firmware --sysbuild`
and uploads the resulting signed images as the `uwb-tag-firmware-signed`
GitHub Actions artifact:

- `build/mcuboot/zephyr/zephyr.hex` — bootloader (SWD flash, §1)
- `build/uwb_tag_firmware/zephyr/zephyr.signed.hex` — signed app for slot0
  (SWD flash, §1)
- `build/uwb_tag_firmware/zephyr/zephyr.signed.bin` — same, raw binary; also
  the file `mcumgr image upload` pushes into slot1 for BLE DFU (§2a)
- `build/dfu_application.zip` — manifest-wrapped equivalent (`.bin` +
  `manifest.json`) for nRF Connect Device Manager (§2b)

Download it from the run's **Actions → (workflow run) → Artifacts** panel to
DFU a tag from a machine without the NCS toolchain installed, or to sanity
check CI's build against the local one before a bench session.

Attaching this artifact to **tagged releases** (in addition to every CI run)
is deferred — not done in UWB-267 — since there is no tagging/release
process yet for this repo; revisit once one exists.

---

## References

- `docs/adr/0040-tag-ota-transport.md` (`FUNDAMENTALS_SPORTS` repo) — the
  design decision this procedure implements.
- `README.md` "MCUboot & OTA" — partition layout, swap-mode rationale, BLE
  transport internals, and the full self-confirm/health-gate honesty note.
- `keys/mcuboot_dev_key.pem` — dev signing key header comment (rotation
  checklist for a real deployment).
- `sysbuild.conf` / `pm_static.yml` — bootloader/swap-mode config and the
  static partition table referenced throughout this doc.
