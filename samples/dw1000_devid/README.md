# DW1000 device-ID bring-up sample (UWB-94)

Minimal bring-up sample that reads the DW1000 DEV\_ID register after reset and
logs PASS when the value matches `0xDECA0130`.  Intended as the first on-hardware
smoke test after the Zephyr SPI/GPIO port layer is flashed (UWB-93).

## What it does

1. `dw1000_port_init()` — configures SPI1 (2 MHz slow / 8 MHz fast), RESET and
   IRQ GPIOs, and registers the deferred-ISR work item.
2. `reset_DW1000()` — asserts RESET low for 10 ms then releases to input.
3. `port_set_dw1000_slowrate()` — switches SPI to 2 MHz (DW1000 requirement during
   `dwt_initialise()`).
4. `dwt_initialise(DWT_LOADNONE)` — internally reads DEV\_ID and returns
   `DWT_ERROR` if it does not match.  This is the earliest point a wiring fault
   surfaces.
5. `port_set_dw1000_fastrate()` — raises SPI to 8 MHz.
6. `dwt_readdevid()` — reads DEV\_ID register (address 0x00, 4 bytes, little-endian)
   and logs PASS / FAIL.

## Building

```
west build -b nrf52dk_nrf52832 samples/dw1000_devid
```

## Flashing

```
west flash
```

## Expected RTT / UART output on a real DWM1001 module

```
*** Booting nRF Connect SDK v2.7.0 ***
[00:00:00.000,000] <inf> dw1000_devid: DW1000 device-ID bring-up sample starting
[00:00:00.012,000] <dbg> deca_port: DW1000 port init OK (slow=2000000 Hz, fast=8000000 Hz)
[00:00:00.028,000] <dbg> deca_port: DW1000 hardware reset done
[00:00:00.028,000] <dbg> deca_port: DW1000 SPI -> slow (2000000 Hz)
[00:00:00.090,000] <inf>  dw1000_devid: PASS: DW1000 DEV_ID = 0xDECA0130 (expected 0xDECA0130)
```

If the DEV\_ID read fails (wrong wiring, SPI speed too fast, board not connected),
the log will show `dwt_initialise failed` or `FAIL: DW1000 DEV_ID = 0x00000000`.

## Headless CI test

The functional equivalent of this sample — using a mocked SPI transport — runs
headlessly in CI via `west twister`:

```
west twister -p unit_testing -T tests/dw1000_devid -v
```

See `tests/dw1000_devid/` for the ztest source.
