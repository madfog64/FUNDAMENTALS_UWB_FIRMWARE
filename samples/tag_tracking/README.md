# TDoA tag tracking (blinker) loop sample (UWB-252)

Runs the tag-side per-cycle TDoA tracking loop (`drivers/dw1000/uwb_tracking.c`)
in a `while (1)`, logging the outcome of every superframe cycle. During Phase-2
tracking (ADR-001 §Phase 2) this is the mode a tag runs once it has joined the
network and been assigned a short address + TDMA slot.

## What it does

1. `dw1000_port_init()` — configures SPI1 (2 MHz slow / 8 MHz fast), RESET and
   IRQ GPIOs (UWB-93).
2. `dw1000_configure()` — full PHY configuration + antenna delay programming
   (UWB-155/UWB-156). This sample assumes it succeeds; a real tag firmware
   handles the same precondition before entering the tracking loop.
3. `uwb_tracking_state_init()` — starts the tag's cycle reference in its
   "never synced" state.
4. Loops calling `uwb_tracking_run_cycle(&cfg, &state, sync_timeout_us)`
   (`drivers/dw1000/uwb_tracking.h`) once per superframe cycle:
   - Listens for the master's SYNC frame (`dw1000_sync_rx`,
     `CONFIG_TAG_TRACKING_SAMPLE_SYNC_TIMEOUT_US`) and updates the cycle
     reference (`uwb_cycle_ref_on_sync` / `uwb_cycle_ref_on_miss`).
   - **Only if the reference is currently valid** (UWB-243's mandatory
     collision-avoidance gate — see `uwb_tracking.h`), computes this tag's
     scheduled blink TX time for its assigned slot (`uwb_slot_tx_time`,
     UWB-251), builds the blink (`uwb_build_tag_blink`, UWB-250), and
     schedules it (`dw1000_tx_at`, UWB-231).
   - Logs the outcome of every cycle (`BLINKED`, `NO_VALID_REF`,
     `MISSED_WINDOW`, `BUILD_ERROR`) together with `cycle_seq` and
     `blink_count`.

`self_addr` / `slot_idx` / `slot_count` / `slot_duration_dtu` are placeholders
(`CONFIG_TAG_TRACKING_SAMPLE_*`, see `Kconfig`) — real slot/address assignment
is the Aloha join/registration flow (UWB-9/10/11), out of scope for this
sample and for UWB-252.

## Building

```
west build -b nrf52dk_nrf52832 samples/tag_tracking
```

## Flashing

```
west flash
```

## On-hardware bring-up procedure

The ztest suite (`tests/tracking/`) exercises `uwb_tracking_run_cycle()`
entirely against a mocked `deca_driver` on the `unit_testing` platform (same
mock/stub pattern as `tests/dw1000_sync/`); it cannot verify real RF timing,
that a real clock-master anchor's SYNC frame is correctly received, or that
the blink actually lands inside the assigned slot on-air. Before relying on
this sample / `drivers/dw1000/uwb_tracking.c` on a DWM1001:

1. **Two boards**: a hand-built SYNC master (a second DWM1001 running a
   throwaway loop that periodically transmits a well-formed
   `uwb_sync_frame_t` via `dw1000_tx_at()` — not part of this repo's
   committed samples, see UWB-242's bring-up note for the frame shape) and a
   tag flashed with this sample.
2. **With the master OFF**, confirm the tag logs `NO_VALID_REF` every cycle
   and never transmits (observe via a logic analyser / spectrum capture, or
   simply the absence of any TX activity on the DW1000):
   ```
   [00:00:0x.xxx,000] <dbg> tag_tracking_sample: blink_count=0: NO_VALID_REF (no valid cycle reference)
   ```
3. **Power on the master.** Confirm the tag transitions to `BLINKED` within a
   few cycles of the first SYNC frame:
   ```
   [00:00:0x.xxx,000] <inf> tag_tracking_sample: cycle_seq=0x0001 blink_count=1: BLINKED
   ```
4. **Confirm the tag's blink lands inside its configured slot**: capture both
   the master's SYNC TX and the tag's blink TX on a logic analyser / spectrum
   capture and confirm the time delta is approximately
   `slot_idx * slot_duration_dtu` (converted to seconds via the DW1000 tick
   rate, ~15.65 ps/tick) — within the guard band folded into
   `slot_duration_dtu` (see `drivers/dw1000/uwb_slot_timing.h`).
5. **Power off (or move out of range) the master again mid-run** and confirm
   the tag goes quiet — logs return to `NO_VALID_REF` — once
   `CONFIG_UWB_SYNC_MAX_MISSED` consecutive cycles have elapsed with no SYNC
   heard, rather than continuing to blink off a stale reference indefinitely.
6. If two or more tags are available, assign each a distinct
   `CONFIG_TAG_TRACKING_SAMPLE_SLOT_IDX` and confirm (via a logic analyser or
   a listener/dumper role, see the old-firmware reference's "Listener"
   concept) that their blinks do not collide on-air.

Record board IDs, firmware git SHA, and pass/fail per step in the PR or a
bring-up log — this sample's PR is build-verified only, not
hardware-verified, until this checklist has been run.

## Headless CI test

```
west twister -p unit_testing -T tests/tracking -v
```

See `tests/tracking/` for the ztest source (mocked `deca_driver`, covering the
never-synced/no-TX gate, the successful-sync build+schedule path,
`blink_count` incrementing only on an actually-transmitted blink, sync-loss
invalidation + re-acquisition, the missed-TX-window `-EIO` path, and
out-of-range slot config rejection).
