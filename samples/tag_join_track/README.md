# Join -> tracking sample (UWB-263)

Runs the join -> tracking orchestration seam (`drivers/dw1000/uwb_join_track.c`)
in a `while (1)`, logging join attempts/backoff while unassigned, the adopted
`SLOT_ASSIGNMENT` once joined, and every tracking cycle's blink outcome
afterwards. This is the mode a tag runs from a cold start / mode entry all
the way through Phase-2 tracking (ADR-001 §Phase 2, ADR-0039).

## What it does differently from `samples/tag_tracking`

`samples/tag_tracking` (UWB-252) injects `self_addr` / `slot_idx` /
`slot_count` / `slot_duration_dtu` as compile-time Kconfig placeholders — real
slot/address assignment was explicitly out of scope for that ticket. This
sample retires that stub: it injects **no slot config at all**. Every value
the blinker uses comes from the `SLOT_ASSIGNMENT` this tag receives at
runtime, via the Aloha join state machine (`uwb_join.c`, UWB-261) and this
ticket's join → track seam (`uwb_join_track.c`, UWB-263). If this tag never
receives a matching assignment, it never leaves the join phase and never
transmits a blink — see `tests/join_track/` for the headless proof of that
property.

## What it does

1. `dw1000_port_init()` — configures SPI1 (2 MHz slow / 8 MHz fast), RESET and
   IRQ GPIOs (UWB-93).
2. `dw1000_configure()` — full PHY configuration + antenna delay programming
   (UWB-155/UWB-156). This sample assumes it succeeds; a real tag firmware
   handles the same precondition before entering the join/tracking loop.
3. `uwb_join_track_state_init()` — starts in the join phase.
4. Loops calling `uwb_join_track_step(&cfg, &state)`
   (`drivers/dw1000/uwb_join_track.h`) once per superframe cycle:
   - **Join phase**: drives `uwb_join_step()` (`uwb_join.h`) — the bounded
     network-alive gate (`CONFIG_TAG_JOIN_TRACK_SAMPLE_GATE_CYCLES`, 0 by
     default = disabled), JOIN_REQUEST TX, the await window for a matching
     SLOT_ASSIGNMENT, and truncated-exponential backoff/retry on timeout.
     Logs every cycle's outcome (`JOIN_REQUEST_SENT`, `JOIN_AWAIT_TIMEOUT`,
     `JOIN_BACKOFF_WAITING`, etc.) at INF level so bring-up progress is
     visible without needing DBG on the whole board.
   - **Adoption**: on a matching `SLOT_ASSIGNMENT`, logs the adopted
     `short_addr` / `slot_idx` / `slot_count` / `slot_duration_dtu` and
     transitions to the track phase.
   - **Track phase**: drives `uwb_tracking_run_cycle()` (`uwb_tracking.h`,
     UWB-252) with the adopted config — listens for the master's SYNC frame,
     updates the cycle reference, and (only if that reference is valid)
     builds + schedules this tag's blink in its adopted TDMA slot. Per
     ADR-0039, an already-joined tag **keeps its assignment across transient
     SYNC loss** — the seam never falls back to the join phase on its own;
     only a fresh boot / mode re-entry does (out of scope for this sample —
     it always starts a fresh run in the join phase).

The three `uwb_join_config_t` callbacks the join state machine needs
(`eui64_get` / `rng` / `now_get`) are wired to real hardware here (see
`src/main.c`'s top-of-file comment):
`dwt_geteui()` (this board's real DW1000 EUI-64 register),
`sys_rand32_get()` (Zephyr's RNG, reduced to a uniform draw), and
`dwt_readsystimestamphi32() << 8` (the DW1000 40-bit system time).

## Building

```
west build -b nrf52dk_nrf52832 samples/tag_join_track
```

## Flashing

```
west flash
```

## On-hardware bring-up procedure

The ztest suite (`tests/join_track/`) exercises `uwb_join_track_step()`
entirely against a mocked `deca_driver` on the `unit_testing` platform; it
cannot verify real RF timing, that a real master-side registrar responds
correctly, or that two tags actually receive distinct assignments over the
air. Before relying on this sample / `drivers/dw1000/uwb_join_track.c` on
DWM1001 hardware:

**Requires**: one anchor running the ADR-0039 master-side registrar +
clock-master SYNC (anchor repo, UWB-260/262) and one or two DWM1001 tags each
flashed with this sample.

1. **With the master/registrar OFF**, power on a tag running this sample.
   Confirm it logs `JOIN_GATE_LISTENING` / `JOIN_REQUEST_SENT` /
   `JOIN_AWAIT_TIMEOUT` / `JOIN_BACKOFF_WAITING` cycling (attempt count
   growing, backoff window growing per ADR-0039's truncated-exponential
   schedule) and **never** transmits a blink (no `BLINKED` log line ever
   appears — confirm via a logic analyser / spectrum capture that every
   transmitted frame is JOIN_REQUEST-length, never TAG_BLINK-length, if
   available):
   ```
   [00:00:0x.xxx,000] <inf> tag_join_track_sample: cycle 3: JOIN_REQUEST_SENT (still joining)
   [00:00:0x.xxx,000] <inf> tag_join_track_sample: cycle 7: JOIN_AWAIT_TIMEOUT (still joining)
   [00:00:0x.xxx,000] <inf> tag_join_track_sample: cycle 8: JOIN_BACKOFF_WAITING (still joining)
   ```
2. **Power on the master/registrar.** Confirm the tag reaches `JOINED` within
   a few attempts and logs the adopted assignment:
   ```
   [00:00:0x.xxx,000] <inf> tag_join_track_sample: cycle 12: JOINED -- adopted short_addr=0x0003 slot=2/24 slot_duration_dtu=31948800 -- entering track phase
   ```
3. **Confirm tracking starts.** Within a few cycles of adoption, confirm the
   tag transitions to `BLINKED`:
   ```
   [00:00:0x.xxx,000] <inf> tag_join_track_sample: cycle 13: blink_count=1: BLINKED
   ```
4. **Confirm the tag's blink lands inside its adopted slot**: capture both the
   master's SYNC TX and the tag's blink TX on a logic analyser / spectrum
   capture and confirm the time delta is approximately
   `slot_idx * slot_duration_dtu` (converted to seconds via the DW1000 tick
   rate, ~15.65 ps/tick) — the SAME check `samples/tag_tracking`'s bring-up
   procedure runs, but here `slot_idx` is whatever the registrar assigned,
   not a hand-configured Kconfig value.
5. **Power off (or move out of range) the master again mid-run** and confirm
   the tag goes quiet (`NO_VALID_REF` cycling, per `uwb_tracking.h`'s
   `CONFIG_UWB_SYNC_MAX_MISSED` tolerance) **without ever logging a
   `JOIN_*` outcome again** — confirms the seam keeps its assignment across
   transient SYNC loss (ADR-0039) rather than re-joining.
6. **Second-tag distinct-slot check**: power on a SECOND tag running this
   sample (deliberate Aloha collision with the first tag's retry timing is
   fine — the randomised backoff should de-correlate them). Confirm BOTH
   tags reach `JOINED` and log **DIFFERENT** `short_addr` / `slot_idx` pairs
   from the registrar's idempotent lowest-free allocation (ADR-0039), and
   that their blinks do not collide on-air (capture both on a logic analyser
   / spectrum capture, or use a listener/dumper role if available — see the
   old-firmware reference's "Listener" concept).

Record board IDs, firmware git SHA, and pass/fail per step in the PR or a
bring-up log — this sample's PR is build-verified only, not
hardware-verified, until this checklist has been run.

## Headless CI test

```
west twister -p unit_testing -T tests/join_track -v
```

See `tests/join_track/` for the ztest source (mocked `deca_driver`), covering
the join → track transition using the ADOPTED config (not any
hard-coded/injected value) for the blink's MAC src address and scheduled slot
timing, a tag that never receives a matching assignment never entering the
track phase (never calling the blink `dw1000_tx_at()`), a foreign-EUI64
assignment not causing a false adopt, and the seam staying in the track phase
across transient SYNC loss rather than re-joining.
