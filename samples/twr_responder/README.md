# DS-TWR responder mode sample (UWB-232)

Runs the DS-TWR responder state machine (`drivers/dw1000/twr_responder.c`) in a
loop, logging each POLL/RESPONSE/FINAL exchange attempt. During Phase-1
calibration (ADR-001 §Phase 1) this is the mode a reference tag (or an
anchor, anchor-side) runs while another device initiates DS-TWR ranging
against it.

## What it does

1. `dw1000_port_init()` — configures SPI1 (2 MHz slow / 8 MHz fast), RESET and
   IRQ GPIOs (UWB-93).
2. `dw1000_configure()` — full PHY configuration + antenna delay programming
   (UWB-155/UWB-156). This sample assumes it succeeds; a real reference tag
   or anchor firmware handles the same precondition before entering responder
   mode.
3. Loops calling `twr_responder_run_once(self_addr, &exchange)`
   (`drivers/dw1000/twr_responder.h`):
   - Arms RX for a POLL (`dw1000_rx`, `CONFIG_UWB_TWR_RESPONDER_POLL_TIMEOUT_US`).
   - On a POLL addressed to `self_addr`, captures T2, computes T3 = T2 +
     `CONFIG_UWB_TWR_RESP_REPLY_DELAY_DTU`, builds and schedules a RESPONSE
     (`uwb_build_twr_response` + `dw1000_tx_at`).
   - Arms RX for the matching FINAL (`CONFIG_UWB_TWR_RESPONDER_FINAL_TIMEOUT_US`),
     extracts T1/T4/T5 and captures T6 on a match.
   - With `CONFIG_UWB_TWR_RESPONDER_DEBUG_RANGE=y` (this sample's default),
     also computes and logs a local range estimate from T1..T6
     (`uwb_twr_range_mm()`) — debug/diagnostic only; per ADR-011 the tag never
     reports these timestamps or the range off-device.
   - Logs the outcome of every attempt (`EXCHANGE_OK`, `NO_POLL`,
     `FOREIGN_FRAME`, `TX_ERROR`, `NO_FINAL`, `FINAL_MISMATCH`).

`self_addr` is a placeholder (`CONFIG_TWR_RESPONDER_SAMPLE_SELF_ADDR`, default
`0x0002`) — real slot/address assignment is the Aloha join/registration flow
(UWB-9/10/11), out of scope for this sample and for UWB-232.

## Building

```
west build -b nrf52dk_nrf52832 samples/twr_responder
```

## Flashing

```
west flash
```

## On-hardware bring-up procedure

The ztest suite (`tests/twr_responder/`) exercises `twr_responder_run_once()`
entirely against a mocked `deca_driver` on the `unit_testing` platform (same
mock as `tests/dw1000_ranging/`); it cannot verify real RF timing or a real
DS-TWR exchange against another board. Before relying on this sample /
`drivers/dw1000/twr_responder.c` on a DWM1001:

1. Two DWM1001 boards, both able to run `dw1000_configure()` successfully
   (confirm with `samples/dw1000_config` first).
2. **Board A ("responder")**: flash this sample. Note the short address it
   logs at boot (`CONFIG_TWR_RESPONDER_SAMPLE_SELF_ADDR`, default `0x0002`).
3. **Board B ("initiator")**: run a DS-TWR initiator that POLLs Board A's
   short address, waits for the RESPONSE, then sends a scheduled FINAL. This
   can be:
   - a full anchor-host DS-TWR initiator (lives in the anchor repo,
     `FUNDAMENTALS_ANCHOR` — out of scope here), or
   - a throwaway initiator sketch built on the same
     `drivers/dw1000/dw1000_ranging.c` + `uwb_twr_codec.c` primitives (not
     part of this repo's committed samples — UWB-232's scope is the responder
     side only).
4. Confirm Board A logs a completed exchange:
   ```
   [00:00:0x.xxx,000] <inf> twr_responder_sample: Exchange 0x0001 with initiator 0x0001 complete: T1=... T2=... T3=... T4=... T5=... T6=...
   [00:00:0x.xxx,000] <inf> twr_responder: DS-TWR exchange 0x0001 with 0x0001: range = NNNN mm
   ```
5. Confirm the logged range is plausible for the known physical separation
   between the two boards (within roughly a metre before antenna-delay
   calibration — ADR-001; this module does not calibrate antenna delay).
6. Move Board A out of range (or power off Board B) and confirm Board A logs
   `NO_POLL` repeatedly rather than hanging.
7. Stop Board B after it sends RESPONSE-RX but before it would send FINAL
   (e.g. kill the initiator process/sketch) and confirm Board A logs
   `NO_FINAL` for that exchange, then resumes listening for the next POLL.

Record board IDs, firmware git SHA, and pass/fail per step in the PR or a
bring-up log — this sample's PR is build-verified only, not
hardware-verified, until this checklist has been run.

## Headless CI test

```
west twister -p unit_testing -T tests/twr_responder -v
```

See `tests/twr_responder/` for the ztest source (mocked `deca_driver`,
covering the POLL/RESPONSE/FINAL happy path, the debug-range formula, and the
timeout / foreign-frame / mismatched-exchange-id paths).
