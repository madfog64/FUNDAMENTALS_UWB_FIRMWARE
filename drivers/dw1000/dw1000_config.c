/*! ----------------------------------------------------------------------------
 * @file    dw1000_config.c
 * @brief   DW1000 PHY radio configuration layer (UWB-155).
 *
 * Implements dw1000_configure() — the single entry point that takes the
 * DW1000 from power-on reset through to a fully configured radio ready for
 * TDoA blink TX and TWR calibration RX.
 *
 * PHY parameter values come from Kconfig (CONFIG_DW1000_PHY_*) whose defaults
 * are locked to contracts/uwb/phy_config.h v0.6 (pinned at
 * drivers/dw1000/phy_config.h).  The compile-time assertions below verify
 * the Kconfig defaults still match the pinned contracts header at build time.
 *
 * Sequence rationale:
 *   reset_DW1000()                bring chip out of reset cleanly
 *   port_set_dw1000_slowrate()    SPI must be <3 MHz during dwt_initialise
 *   dwt_initialise(DWT_LOADUCODE) reads DEV_ID, loads LDE microcode from ROM
 *                                 (required for accurate RX timestamps — unlike
 *                                 the DWT_LOADNONE used in the devid sample)
 *   port_set_dw1000_fastrate()    raise SPI to 8 MHz for normal operation
 *   dwt_configure(&cfg)           write channel/PRF/PLEN/PAC/codes/SFD/
 *                                 data-rate/PHR-mode/sfdTO
 *   dwt_configuretxrf(&txcfg)     write PG delay + TX power
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <errno.h>

#include <zephyr/logging/log.h>

#include "deca_device_api.h"
#include "dw1000_config.h"
#include "phy_config.h"

/*
 * Port functions — provided by drivers/dw1000/port/deca_port.c in the
 * Zephyr library build and by mock_port.c in the unit_testing build.
 * Forward-declared here rather than including deca_port.h to avoid pulling
 * in the Zephyr SPI/GPIO driver headers in non-Zephyr test environments.
 */
extern void reset_DW1000(void);
extern void port_set_dw1000_slowrate(void);
extern void port_set_dw1000_fastrate(void);

LOG_MODULE_REGISTER(dw1000_config, LOG_LEVEL_DBG);

/* ---------------------------------------------------------------------------
 * Compile-time verification that the Kconfig defaults match phy_config.h.
 *
 * These assertions catch divergence between the Kconfig defaults (which
 * anchor/contracts tooling diffs against) and the pinned phy_config.h header.
 * They fire at build time — fix the Kconfig default, never silence the assert.
 * --------------------------------------------------------------------------- */

_Static_assert(CONFIG_DW1000_PHY_CHANNEL == UWB_PHY_CHANNEL,
    "CONFIG_DW1000_PHY_CHANNEL default does not match UWB_PHY_CHANNEL in "
    "phy_config.h — update Kconfig default to keep them in sync");

_Static_assert(CONFIG_DW1000_PHY_PRF == UWB_PHY_PRF,
    "CONFIG_DW1000_PHY_PRF default does not match UWB_PHY_PRF in phy_config.h");

_Static_assert(CONFIG_DW1000_PHY_TX_PLEN == UWB_PHY_TX_PLEN,
    "CONFIG_DW1000_PHY_TX_PLEN default does not match UWB_PHY_TX_PLEN in "
    "phy_config.h");

_Static_assert(CONFIG_DW1000_PHY_PAC == UWB_PHY_PAC,
    "CONFIG_DW1000_PHY_PAC default does not match UWB_PHY_PAC in phy_config.h");

_Static_assert(CONFIG_DW1000_PHY_TX_CODE == UWB_PHY_TX_CODE,
    "CONFIG_DW1000_PHY_TX_CODE default does not match UWB_PHY_TX_CODE in "
    "phy_config.h");

_Static_assert(CONFIG_DW1000_PHY_RX_CODE == UWB_PHY_RX_CODE,
    "CONFIG_DW1000_PHY_RX_CODE default does not match UWB_PHY_RX_CODE in "
    "phy_config.h");

_Static_assert(CONFIG_DW1000_PHY_NS_SFD == UWB_PHY_NS_SFD,
    "CONFIG_DW1000_PHY_NS_SFD default does not match UWB_PHY_NS_SFD in "
    "phy_config.h");

_Static_assert(CONFIG_DW1000_PHY_DATA_RATE == UWB_PHY_DATA_RATE,
    "CONFIG_DW1000_PHY_DATA_RATE default does not match UWB_PHY_DATA_RATE in "
    "phy_config.h");

_Static_assert(CONFIG_DW1000_PHY_PHR_MODE == UWB_PHY_PHR_MODE,
    "CONFIG_DW1000_PHY_PHR_MODE default does not match UWB_PHY_PHR_MODE in "
    "phy_config.h");

_Static_assert(CONFIG_DW1000_PHY_SFD_TIMEOUT == UWB_PHY_SFD_TIMEOUT,
    "CONFIG_DW1000_PHY_SFD_TIMEOUT default does not match UWB_PHY_SFD_TIMEOUT "
    "in phy_config.h");

_Static_assert(CONFIG_DW1000_PHY_TX_PGDELAY == UWB_PHY_TX_PGDELAY,
    "CONFIG_DW1000_PHY_TX_PGDELAY default does not match UWB_PHY_TX_PGDELAY "
    "in phy_config.h");

/* ---------------------------------------------------------------------------
 * dw1000_configure
 * --------------------------------------------------------------------------- */
int dw1000_configure(void)
{
    int ret;

    /* Step 1 — Hardware reset.
     *
     * Drives RESET low for 10 ms then releases to input.  Required to bring
     * the DW1000 into a clean, known state before the driver initialises it.
     */
    reset_DW1000();

    /* Step 2 — Slow SPI rate.
     *
     * DW1000 User Manual section 2.3.2: SPI clock must be < 3 MHz during
     * dwt_initialise().  The port layer starts at slow rate; this call is
     * explicit so the sequence is self-documenting.
     */
    port_set_dw1000_slowrate();

    /* Step 3 — Initialise the DW1000 driver with LDE microcode.
     *
     * DWT_LOADUCODE uploads the Leading-edge Detection algorithm from ROM.
     * This is required for accurate hardware RX timestamps (critical for
     * TDoA and DS-TWR ranging).  The devid bring-up sample uses DWT_LOADNONE
     * (sufficient for a device-ID read only) — this is the upgrade.
     *
     * dwt_initialise reads DEV_ID internally; it returns DWT_ERROR if the
     * SPI is misconfigured or the DW1000 is not present.
     */
    ret = dwt_initialise(DWT_LOADUCODE);
    if (ret != DWT_SUCCESS) {
        LOG_ERR("dwt_initialise(DWT_LOADUCODE) failed (%d) — "
                "check SPI wiring and slow rate", ret);
        return -EIO;
    }
    LOG_DBG("dwt_initialise(DWT_LOADUCODE) OK");

    /* Step 4 — Fast SPI rate.
     *
     * Safe to raise the SPI clock after a successful dwt_initialise().
     */
    port_set_dw1000_fastrate();

    /* Step 5 — Configure the PHY radio parameters.
     *
     * All values from CONFIG_DW1000_PHY_* (see Kconfig) whose defaults
     * match contracts/uwb/phy_config.h v0.6 exactly.  The _Static_assert
     * checks above guarantee the Kconfig defaults are in sync at build time.
     *
     * Key choices (see contracts/uwb/README.md §Rationale for full detail):
     *   channel 5     — 6489.6 MHz, solid global regulatory coverage
     *   PRF 64 MHz    — superior first-path / NLOS performance
     *   PLEN 64       — 64 symbols, airtime ~109 us (< 150 us blink budget)
     *   PAC 8         — required for PLEN <= 128 (DW1000 User Manual)
     *   code 9/9      — standard Ch5+PRF64M code, orthogonal to 10/11/12
     *   std SFD       — IEEE 802.15.4a compliant; same length as Decawave NS
     *   6.8 Mbps      — only rate that keeps payload within 150 us budget
     *   STD PHR       — frames <= 127 bytes (all uwb_frames.h types qualify)
     *   sfdTO = 65    — 64 + 1 + 8 - 8 = 65 (DW1000 UM recommended formula)
     */
    dwt_config_t cfg = {
        .chan           = CONFIG_DW1000_PHY_CHANNEL,
        .prf            = CONFIG_DW1000_PHY_PRF,
        .txPreambLength = CONFIG_DW1000_PHY_TX_PLEN,
        .rxPAC          = CONFIG_DW1000_PHY_PAC,
        .txCode         = CONFIG_DW1000_PHY_TX_CODE,
        .rxCode         = CONFIG_DW1000_PHY_RX_CODE,
        .nsSFD          = CONFIG_DW1000_PHY_NS_SFD,
        .dataRate       = CONFIG_DW1000_PHY_DATA_RATE,
        .phrMode        = CONFIG_DW1000_PHY_PHR_MODE,
        .sfdTO          = (uint16)CONFIG_DW1000_PHY_SFD_TIMEOUT,
    };
    dwt_configure(&cfg);

    LOG_DBG("DW1000 PHY: ch=%u PRF=%u PLEN=0x%02X PAC=%u "
            "code=%u/%u nsSFD=%u rate=%u PHR=%u sfdTO=%u",
            cfg.chan, cfg.prf, (unsigned)cfg.txPreambLength, cfg.rxPAC,
            cfg.txCode, cfg.rxCode, cfg.nsSFD, cfg.dataRate,
            cfg.phrMode, cfg.sfdTO);

    /* Step 6 — Configure TX power profile.
     *
     * PGdly: TC_PGDELAY_CH5 = 0xC0, the Decawave-specified pulse generator
     *        delay calibration constant for channel 5.  Do not change without
     *        RF spectrum analysis.
     *
     * power: CONFIG_DW1000_PHY_TX_POWER defaults to UWB_PHY_TX_POWER_NOMINAL
     *        (0x0E082848 — non-boosted, dev / blank OTP boards only).
     *
     * Production note: replace CONFIG_DW1000_PHY_TX_POWER with the factory-
     * calibrated OTP value.  Read it via dwt_read32bitreg(TX_POWER_ID) after
     * dwt_initialise() completes; non-zero values indicate a programmed OTP.
     * Using the factory value is required for regulatory compliance.
     */
    dwt_txconfig_t txcfg = {
        .PGdly = (uint8)CONFIG_DW1000_PHY_TX_PGDELAY,
        .power = (uint32)CONFIG_DW1000_PHY_TX_POWER,
    };
    dwt_configuretxrf(&txcfg);

    LOG_DBG("DW1000 TX: PGdly=0x%02X power=0x%08X",
            (unsigned)txcfg.PGdly, (unsigned)txcfg.power);

    return 0;
}
