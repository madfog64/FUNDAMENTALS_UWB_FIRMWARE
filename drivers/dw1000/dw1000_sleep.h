/*! ----------------------------------------------------------------------------
 * @file    dw1000_sleep.h
 * @brief   DW1000 driver-level sleep/wake seam (UWB-316).
 *
 * Per ADR-0046 ("Tag power-management profile & low-power state model"),
 * STANDBY's real current savings come from deep-sleeping the DW1000. This
 * module is the *mechanism* the STANDBY state machine (UWB-319) drives: it
 * does NOT decide when to sleep or wake, does not run a wake-cadence timer,
 * and does not know about power states — it only wraps the deca_driver sleep
 * API + the WAKEUP GPIO with correct call ordering.
 *
 *   dw1000_sleep()   dwt_configuresleep() + dwt_entersleep() — puts the
 *                    DW1000 into DEEPSLEEP.
 *   dw1000_wake()    Toggles the WAKEUP pin (or falls back to a no-op if the
 *                    board has none wired — see port_wakeup_dw1000() in
 *                    port/deca_port.c), then re-establishes a usable radio
 *                    via dw1000_configure() (UWB-155/UWB-156) — the DW1000
 *                    loses its full register configuration across deep sleep
 *                    (only the small AON array survives), so the caller gets
 *                    back a radio in exactly the same state dw1000_configure()
 *                    always produces: dwt_initialise() (which re-reads
 *                    DEV_ID), dwt_configure(), dwt_configuretxrf(), and
 *                    antenna-delay restore (OTP or Kconfig fallback).
 *
 * Both functions are synchronous and block for the tens-of-milliseconds the
 * DW1000 needs to enter/leave deep sleep (see the timing notes on
 * port_wakeup_dw1000() in deca_port.h) — callers on a real tag are expected
 * to invoke these only from STANDBY/OFF transitions (ADR-0046), never from
 * the tight ACTIVE tracking cycle.
 *
 * Out of scope (see ADR-0046 §"New work created"):
 *   - The STANDBY power-state machine + wake cadence policy (UWB-319).
 *   - The CONFIG_UWB_TAG_LOW_POWER profile Kconfig flag (UWB-314).
 *   - BLE advertising duty-cycle policy (UWB-320).
 *   - Battery monitoring (UWB-322).
 *
 * -----------------------------------------------------------------------
 * On-hardware bring-up (manual — not automated by the ztest suite)
 * -----------------------------------------------------------------------
 * The ztest suite (tests/dw1000_sleep/) exercises the call sequence entirely
 * against a mocked deca_driver on the 'unit_testing' platform; it cannot
 * verify that the DW1000 actually enters DEEPSLEEP or that the WAKEUP pin
 * pulse is electrically correct. Before relying on this module on a DWM1001:
 *
 *   1. Confirm the board overlay wires wakeup-gpios under /{ zephyr,user {}; }
 *      to a spare GPIO connected to the DW1000 WAKEUP pin (module pin 8 on
 *      DWM1001 — confirm against the actual carrier board schematic; the
 *      pin used in boards/nrf52dk_nrf52832.overlay here is a placeholder for
 *      the nrf52dk_nrf52832 stand-in target, not a verified DWM1001 trace).
 *   2. dw1000_configure() (bring-up), then dw1000_sleep(). Confirm on a
 *      current probe / logic analyser that DW1000 supply current drops to
 *      the DEEPSLEEP level (DW1000 datasheet) and SPI reads no longer
 *      respond (device is asleep).
 *   3. dw1000_wake(). Confirm the WAKEUP pin pulses, current returns to
 *      active levels, and the function returns DW1000_SLEEP_OK.
 *   4. Immediately after step 3, exercise dw1000_rx()/dw1000_tx_at()
 *      (dw1000_ranging.c) or the blinker and confirm the radio behaves
 *      exactly as after a fresh dw1000_configure() call (no residual
 *      DEEPSLEEP artefacts).
 *
 *   Record board IDs, firmware git SHA, and pass/fail per step in the PR or a
 *   bring-up log — this module's PR is build-verified only, not
 *   hardware-verified, until this checklist has been run.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef DW1000_SLEEP_H_
#define DW1000_SLEEP_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Outcome of dw1000_sleep() / dw1000_wake().
 */
typedef enum {
    /** Sleep/wake sequence completed. */
    DW1000_SLEEP_OK = 0,

    /** dw1000_wake(): the post-wake reconfigure (dw1000_configure(), which
     *  wraps dwt_initialise()) failed — the DW1000 did not respond correctly
     *  after the WAKEUP pulse (SPI fault, device not woken, or the WAKEUP
     *  GPIO is not wired on this board). The radio is NOT usable; the caller
     *  should not proceed to TX/RX. */
    DW1000_SLEEP_ERR_WAKE_RECONFIGURE,
} dw1000_sleep_result_t;

/**
 * @brief Configure and enter DW1000 DEEPSLEEP.
 *
 * Sequence:
 *   1. dwt_configuresleep(mode, wake) — programs the on-wake restore set
 *      (DWT_PRESRV_SLEEP | DWT_CONFIG: reload the AON-preserved config on
 *      wake) and the wake trigger (DWT_WAKE_WK | DWT_WAKE_CS | DWT_SLP_EN:
 *      wake on the WAKEUP pin OR a SPI chip-select pulse, sleep enabled).
 *   2. dwt_entersleep() — the device enters DEEPSLEEP immediately.
 *
 * After this call the DW1000 will not respond to SPI transactions until
 * woken (dw1000_wake()) — do not call any other dw1000_ or dwt_ function
 * until dw1000_wake() has returned DW1000_SLEEP_OK.
 *
 * Must be called from a thread context (not ISR) — dwt_entersleep() issues a
 * SPI write.
 *
 * @return  DW1000_SLEEP_OK. (dwt_configuresleep()/dwt_entersleep() are
 *          void in the deca_driver API; this function cannot itself detect a
 *          failed sleep entry — the DW1000 datasheet gives no SPI-readable
 *          confirmation of DEEPSLEEP entry short of measuring supply
 *          current.)
 */
dw1000_sleep_result_t dw1000_sleep(void);

/**
 * @brief Wake the DW1000 from DEEPSLEEP and re-establish a usable radio.
 *
 * Sequence:
 *   1. port_wakeup_dw1000() (deca_port.h) — pulses the WAKEUP GPIO (or is a
 *      logged no-op if the board has none wired).
 *   2. dw1000_configure() (dw1000_config.h) — the DW1000 loses its register
 *      configuration across DEEPSLEEP (only the small AON array survives, and
 *      only when DWT_PRESRV_SLEEP|DWT_CONFIG were set by dw1000_sleep()), so
 *      the device must be fully reconfigured: reset, dwt_initialise()
 *      (re-reads DEV_ID — the first SPI transaction after waking, and the
 *      one most likely to fail if the wake did not succeed), dwt_configure(),
 *      dwt_configuretxrf(), and antenna-delay restore.
 *
 * Must be called from a thread context (not ISR).
 *
 * @return  DW1000_SLEEP_OK on success — the DW1000 is fully reconfigured and
 *          ready for TX/RX, identical to the state after a fresh
 *          dw1000_configure() call.
 * @return  DW1000_SLEEP_ERR_WAKE_RECONFIGURE if dw1000_configure() failed
 *          after the wake pulse — the radio is not usable.
 */
dw1000_sleep_result_t dw1000_wake(void);

#ifdef __cplusplus
}
#endif

#endif /* DW1000_SLEEP_H_ */
