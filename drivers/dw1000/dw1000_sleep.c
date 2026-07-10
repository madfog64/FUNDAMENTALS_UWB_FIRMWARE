/*! ----------------------------------------------------------------------------
 * @file    dw1000_sleep.c
 * @brief   DW1000 driver-level sleep/wake seam (UWB-316).
 *
 * See dw1000_sleep.h for the full seam contract, scope, and the on-hardware
 * bring-up checklist.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <zephyr/logging/log.h>

#include "deca_device_api.h"
#include "dw1000_config.h"
#include "dw1000_sleep.h"

/*
 * Port function — provided by drivers/dw1000/port/deca_port.c in the Zephyr
 * library build and by a mock in the unit_testing build. Forward-declared
 * here (rather than including deca_port.h) to avoid pulling in the Zephyr
 * SPI/GPIO driver headers in non-Zephyr test environments, mirroring
 * dw1000_config.c's port_set_dw1000_slowrate()/port_set_dw1000_fastrate()
 * forward declarations.
 */
extern void port_wakeup_dw1000(void);

LOG_MODULE_REGISTER(dw1000_sleep, LOG_LEVEL_DBG);

/* ---------------------------------------------------------------------------
 * dwt_configuresleep() parameters.
 *
 * mode (on-wake restore set):
 *   DWT_PRESRV_SLEEP (0x0100) — preserve the sleep configuration itself
 *                               across the sleep/wake cycle.
 *   DWT_CONFIG       (0x0040) — on wake, reload the AON-preserved register
 *                               set (ONW_LDC) back into the HIF. This is the
 *                               "the DW1000 loses config on deep sleep"
 *                               mitigation referenced in ADR-0046 — it
 *                               restores what the small AON array can hold,
 *                               NOT the full PHY configuration. dw1000_wake()
 *                               still runs the full dw1000_configure()
 *                               reconfigure path below because the AON array
 *                               does not cover everything dw1000_configure()
 *                               programs (e.g. antenna delay, TX power).
 *
 * wake (wake trigger):
 *   DWT_WAKE_WK (0x2) — wake on the WAKEUP pin (the primary mechanism this
 *                       ticket wires — see port_wakeup_dw1000()).
 *   DWT_WAKE_CS (0x4) — also wake on a SPI chip-select pulse (dwt_spicswakeup()
 *                       in the vendored driver), kept as a belt-and-braces
 *                       fallback for boards where the WAKEUP GPIO is not
 *                       wired (port_wakeup_dw1000() then only logs a warning
 *                       and issues no pulse — a subsequent SPI transaction
 *                       still wakes the part).
 *   DWT_SLP_EN  (0x1) — enable sleep/deep-sleep functionality at all; without
 *                       this bit dwt_entersleep() has no effect.
 * --------------------------------------------------------------------------- */
#define DW1000_SLEEP_ON_WAKE_MODE ((uint16)(DWT_PRESRV_SLEEP | DWT_CONFIG))
#define DW1000_SLEEP_WAKE_TRIGGER ((uint8)(DWT_WAKE_WK | DWT_WAKE_CS | DWT_SLP_EN))

/* ---------------------------------------------------------------------------
 * dw1000_sleep
 * --------------------------------------------------------------------------- */
dw1000_sleep_result_t dw1000_sleep(void)
{
    /* Step 1 — Program the on-wake restore set and wake trigger.
     *
     * Must be called before dwt_entersleep() (deca_device_api.h contract).
     */
    dwt_configuresleep(DW1000_SLEEP_ON_WAKE_MODE, DW1000_SLEEP_WAKE_TRIGGER);

    /* Step 2 — Enter DEEPSLEEP.
     *
     * Takes effect immediately; the DW1000 will not respond to SPI until
     * woken via dw1000_wake().
     */
    dwt_entersleep();

    LOG_DBG("DW1000 DEEPSLEEP entered (on-wake mode=0x%04X wake trigger=0x%02X)",
            (unsigned)DW1000_SLEEP_ON_WAKE_MODE, (unsigned)DW1000_SLEEP_WAKE_TRIGGER);

    return DW1000_SLEEP_OK;
}

/* ---------------------------------------------------------------------------
 * dw1000_wake
 * --------------------------------------------------------------------------- */
dw1000_sleep_result_t dw1000_wake(void)
{
    int ret;

    /* Step 1 — Pulse the WAKEUP pin.
     *
     * port_wakeup_dw1000() (deca_port.c) drives the WAKEUP GPIO high for the
     * DW1000's minimum wake-pulse duration, then waits for the crystal
     * oscillator to start and stabilise before any SPI transaction is
     * attempted (mirrors the vendored driver's dwt_spicswakeup() timing —
     * see deca_port.h for the exact figures). If the board has no WAKEUP
     * GPIO wired, this is a logged no-op — dw1000_configure() below still
     * issues SPI transactions, which is itself a valid wake trigger
     * (DWT_WAKE_CS, programmed by dw1000_sleep() above).
     */
    port_wakeup_dw1000();

    /* Step 2 — Reconfigure.
     *
     * The DW1000 loses its full register configuration across DEEPSLEEP; only
     * the small AON array survives (and only the subset dw1000_sleep()'s
     * DWT_CONFIG bit restores automatically). dw1000_configure() is the
     * single source of truth for bringing the radio back to a known-good,
     * fully configured state — reset, dwt_initialise() (re-reads DEV_ID, the
     * first SPI transaction after waking and the one most likely to fail if
     * the wake did not succeed), dwt_configure(), dwt_configuretxrf(), and
     * antenna-delay restore (OTP or Kconfig fallback). Reusing it here
     * (rather than duplicating a partial reconfigure) keeps exactly one
     * tested code path for "DW1000 is fully configured".
     */
    ret = dw1000_configure();
    if (ret != 0) {
        LOG_ERR("DW1000 wake: post-wake reconfigure failed (%d)", ret);
        return DW1000_SLEEP_ERR_WAKE_RECONFIGURE;
    }

    LOG_DBG("DW1000 woken and reconfigured");

    return DW1000_SLEEP_OK;
}
