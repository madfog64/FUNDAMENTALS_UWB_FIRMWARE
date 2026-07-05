/*! ----------------------------------------------------------------------------
 * @file    test_dw1000_config.c
 * @brief   ztest suite: DW1000 PHY configuration (UWB-155, UWB-156)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI).
 *
 * Tests in this suite verify that dw1000_configure() drives the deca_driver
 * API with the exact PHY parameter values mandated by contracts/uwb/phy_config.h
 * version 0.6 (pinned at drivers/dw1000/phy_config.h).
 *
 * UWB-156 tests (9, 10, 11) verify antenna delay hook behaviour:
 *   9. Kconfig defaults used when OTP word at 0x1C is zero (unprogrammed).
 *  10. OTP PRF-64 half (bits[31:16]) overrides Kconfig defaults when non-zero.
 *  11. dw1000_set_antenna_delay() overrides TX/RX independently at runtime.
 *
 * Mocked functions (in mock_deca_driver.c and mock_port.c):
 *   dwt_initialise()           — captures config arg (DWT_LOADUCODE vs DWT_LOADNONE)
 *   dwt_configure()            — captures full dwt_config_t struct
 *   dwt_configuretxrf()        — captures dwt_txconfig_t struct
 *   dwt_settxantennadelay()    — captures TX delay + call count (UWB-156)
 *   dwt_setrxantennadelay()    — captures RX delay + call count (UWB-156)
 *   dwt_otpread()              — returns configurable mock_otp_state (UWB-156)
 *   reset_DW1000()             — call counter
 *   port_set_dw1000_slowrate() — call counter
 *   port_set_dw1000_fastrate() — call counter
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <zephyr/ztest.h>

#include "deca_device_api.h"
#include "dw1000_config.h"
#include "phy_config.h"
#include "mock_deca_driver.h"
#include "mock_port.h"

/* ---------------------------------------------------------------------------
 * Suite setup / teardown
 * --------------------------------------------------------------------------- */
static void *suite_setup(void)
{
    return NULL;
}

static void before_each(void *fixture)
{
    (void)fixture;
    mock_deca_reset();
    mock_port_reset();
}

ZTEST_SUITE(dw1000_config, NULL, suite_setup, before_each, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Test 1 — dwt_initialise is called with DWT_LOADUCODE
 *
 * dw1000_configure() must load the LDE microcode (DWT_LOADUCODE = 0x1), not
 * DWT_LOADNONE (0x0).  LDE loading is required for accurate hardware RX
 * timestamps in TDoA and DS-TWR operation.
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_config, test_initialise_called_with_loaducode)
{
    int ret = dw1000_configure();

    zassert_equal(ret, 0,
        "dw1000_configure() returned %d (expected 0 = success)", ret);

    zassert_equal(mock_init_state.called, 1,
        "Expected dwt_initialise() to be called exactly once; got %d",
        mock_init_state.called);

    zassert_equal(mock_init_state.config_arg, (uint16)DWT_LOADUCODE,
        "Expected dwt_initialise(DWT_LOADUCODE=0x%04X); "
        "got dwt_initialise(0x%04X)",
        (unsigned)DWT_LOADUCODE, (unsigned)mock_init_state.config_arg);
}

/* ---------------------------------------------------------------------------
 * Test 2 — dwt_configure is called with the pinned channel and PRF
 *
 * Channel 5 (UWB_PHY_CHANNEL) and PRF 64 MHz (UWB_PHY_PRF = DWT_PRF_64M).
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_config, test_configure_channel_and_prf)
{
    (void)dw1000_configure();

    zassert_equal(mock_cfg_state.called, 1,
        "Expected dwt_configure() to be called exactly once; got %d",
        mock_cfg_state.called);

    zassert_equal(mock_cfg_state.config.chan, (uint8)UWB_PHY_CHANNEL,
        "chan: expected %u (UWB_PHY_CHANNEL); got %u",
        (unsigned)UWB_PHY_CHANNEL, (unsigned)mock_cfg_state.config.chan);

    zassert_equal(mock_cfg_state.config.prf, (uint8)UWB_PHY_PRF,
        "prf: expected %u (UWB_PHY_PRF = DWT_PRF_64M); got %u",
        (unsigned)UWB_PHY_PRF, (unsigned)mock_cfg_state.config.prf);
}

/* ---------------------------------------------------------------------------
 * Test 3 — dwt_configure is called with the pinned preamble length and PAC
 *
 * PLEN 64 (UWB_PHY_TX_PLEN = DWT_PLEN_64 = 0x04) and PAC 8 (UWB_PHY_PAC = 0).
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_config, test_configure_plen_and_pac)
{
    (void)dw1000_configure();

    zassert_equal(mock_cfg_state.config.txPreambLength, (uint8)UWB_PHY_TX_PLEN,
        "txPreambLength: expected 0x%02X (UWB_PHY_TX_PLEN = DWT_PLEN_64); "
        "got 0x%02X",
        (unsigned)UWB_PHY_TX_PLEN, (unsigned)mock_cfg_state.config.txPreambLength);

    zassert_equal(mock_cfg_state.config.rxPAC, (uint8)UWB_PHY_PAC,
        "rxPAC: expected %u (UWB_PHY_PAC = DWT_PAC8); got %u",
        (unsigned)UWB_PHY_PAC, (unsigned)mock_cfg_state.config.rxPAC);
}

/* ---------------------------------------------------------------------------
 * Test 4 — dwt_configure is called with the pinned preamble codes and SFD
 *
 * TX code 9, RX code 9 (both UWB_PHY_TX/RX_CODE); standard SFD (nsSFD = 0).
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_config, test_configure_codes_and_sfd)
{
    (void)dw1000_configure();

    zassert_equal(mock_cfg_state.config.txCode, (uint8)UWB_PHY_TX_CODE,
        "txCode: expected %u (UWB_PHY_TX_CODE); got %u",
        (unsigned)UWB_PHY_TX_CODE, (unsigned)mock_cfg_state.config.txCode);

    zassert_equal(mock_cfg_state.config.rxCode, (uint8)UWB_PHY_RX_CODE,
        "rxCode: expected %u (UWB_PHY_RX_CODE); got %u",
        (unsigned)UWB_PHY_RX_CODE, (unsigned)mock_cfg_state.config.rxCode);

    zassert_equal(mock_cfg_state.config.nsSFD, (uint8)UWB_PHY_NS_SFD,
        "nsSFD: expected %u (UWB_PHY_NS_SFD = standard); got %u",
        (unsigned)UWB_PHY_NS_SFD, (unsigned)mock_cfg_state.config.nsSFD);
}

/* ---------------------------------------------------------------------------
 * Test 5 — dwt_configure is called with the pinned data rate, PHR mode, sfdTO
 *
 * Data rate: 6.8 Mbps (UWB_PHY_DATA_RATE = DWT_BR_6M8 = 2).
 * PHR mode:  standard (UWB_PHY_PHR_MODE = DWT_PHRMODE_STD = 0).
 * sfdTO:     65 symbols (UWB_PHY_SFD_TIMEOUT = 64+1+8-8 = 65).
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_config, test_configure_datarate_phrmode_sfdto)
{
    (void)dw1000_configure();

    zassert_equal(mock_cfg_state.config.dataRate, (uint8)UWB_PHY_DATA_RATE,
        "dataRate: expected %u (UWB_PHY_DATA_RATE = DWT_BR_6M8); got %u",
        (unsigned)UWB_PHY_DATA_RATE, (unsigned)mock_cfg_state.config.dataRate);

    zassert_equal(mock_cfg_state.config.phrMode, (uint8)UWB_PHY_PHR_MODE,
        "phrMode: expected %u (UWB_PHY_PHR_MODE = DWT_PHRMODE_STD); got %u",
        (unsigned)UWB_PHY_PHR_MODE, (unsigned)mock_cfg_state.config.phrMode);

    zassert_equal(mock_cfg_state.config.sfdTO, (uint16)UWB_PHY_SFD_TIMEOUT,
        "sfdTO: expected %u (UWB_PHY_SFD_TIMEOUT = 65); got %u",
        (unsigned)UWB_PHY_SFD_TIMEOUT, (unsigned)mock_cfg_state.config.sfdTO);
}

/* ---------------------------------------------------------------------------
 * Test 6 — dwt_configuretxrf is called with the pinned PG delay
 *
 * PGdly: 0xC0 (UWB_PHY_TX_PGDELAY = TC_PGDELAY_CH5).
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_config, test_configuretxrf_pgdelay)
{
    (void)dw1000_configure();

    zassert_equal(mock_txrf_state.called, 1,
        "Expected dwt_configuretxrf() to be called exactly once; got %d",
        mock_txrf_state.called);

    zassert_equal(mock_txrf_state.config.PGdly, (uint8)UWB_PHY_TX_PGDELAY,
        "PGdly: expected 0x%02X (UWB_PHY_TX_PGDELAY = TC_PGDELAY_CH5); "
        "got 0x%02X",
        (unsigned)UWB_PHY_TX_PGDELAY, (unsigned)mock_txrf_state.config.PGdly);
}

/* ---------------------------------------------------------------------------
 * Test 7 — port call sequence: reset, slowrate, fastrate all called once
 *
 * dw1000_configure() must call reset_DW1000() before dwt_initialise(),
 * port_set_dw1000_slowrate() before dwt_initialise(), and
 * port_set_dw1000_fastrate() after dwt_initialise() succeeds.
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_config, test_port_call_sequence)
{
    (void)dw1000_configure();

    zassert_equal(mock_port_state.reset_called, 1,
        "Expected reset_DW1000() to be called once; got %d",
        mock_port_state.reset_called);

    zassert_equal(mock_port_state.slowrate_called, 1,
        "Expected port_set_dw1000_slowrate() to be called once; got %d",
        mock_port_state.slowrate_called);

    zassert_equal(mock_port_state.fastrate_called, 1,
        "Expected port_set_dw1000_fastrate() to be called once; got %d",
        mock_port_state.fastrate_called);
}

/* ---------------------------------------------------------------------------
 * Test 8 — dw1000_configure() returns -EIO when dwt_initialise fails
 *
 * When dwt_initialise returns DWT_ERROR (SPI fault, wrong device), the
 * function must return -EIO and must NOT proceed to dwt_configure().
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_config, test_init_failure_returns_eio)
{
    /* Pre-configure the mock to simulate a device-not-found error. */
    mock_init_state.return_value = DWT_ERROR;

    int ret = dw1000_configure();

    zassert_equal(ret, -EIO,
        "Expected dw1000_configure() to return -EIO on dwt_initialise failure; "
        "got %d", ret);

    /* dwt_configure must NOT be called when initialisation fails. */
    zassert_equal(mock_cfg_state.called, 0,
        "dwt_configure() must not be called when dwt_initialise fails; "
        "was called %d times", mock_cfg_state.called);
}

/* ---------------------------------------------------------------------------
 * Test 9 — Antenna delay: Kconfig defaults used when OTP is unprogrammed
 *
 * When dwt_otpread() returns 0 for the antenna-delay OTP address (0x1C),
 * dw1000_configure() must call BOTH dwt_settxantennadelay() and
 * dwt_setrxantennadelay() with the Kconfig compile-time defaults
 * (CONFIG_DW1000_ANTENNA_DELAY_TX and CONFIG_DW1000_ANTENNA_DELAY_RX).
 *
 * Acceptance criterion (a) from UWB-156.
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_config, test_antenna_delay_kconfig_default_when_otp_empty)
{
    /* OTP returns 0 — the default state after mock_deca_reset(). */
    /* mock_otp_state.antdly_word is already 0 at this point. */

    int ret = dw1000_configure();

    zassert_equal(ret, 0,
        "dw1000_configure() returned %d (expected 0)", ret);

    /* TX antenna delay must be called exactly once with the Kconfig default. */
    zassert_equal(mock_antenna_state.tx_called, 1,
        "Expected dwt_settxantennadelay() to be called once; got %d",
        mock_antenna_state.tx_called);

    zassert_equal(mock_antenna_state.tx_delay,
        (uint16)CONFIG_DW1000_ANTENNA_DELAY_TX,
        "TX delay: expected Kconfig default %u; got %u",
        (unsigned)CONFIG_DW1000_ANTENNA_DELAY_TX,
        (unsigned)mock_antenna_state.tx_delay);

    /* RX antenna delay must be called exactly once with the Kconfig default. */
    zassert_equal(mock_antenna_state.rx_called, 1,
        "Expected dwt_setrxantennadelay() to be called once; got %d",
        mock_antenna_state.rx_called);

    zassert_equal(mock_antenna_state.rx_delay,
        (uint16)CONFIG_DW1000_ANTENNA_DELAY_RX,
        "RX delay: expected Kconfig default %u; got %u",
        (unsigned)CONFIG_DW1000_ANTENNA_DELAY_RX,
        (unsigned)mock_antenna_state.rx_delay);
}

/* ---------------------------------------------------------------------------
 * Test 10 — Antenna delay: OTP PRF-64 value overrides Kconfig defaults
 *
 * When the OTP word at address 0x1C has a non-zero PRF-64 half (bits[31:16]),
 * dw1000_configure() must use THAT value for both dwt_settxantennadelay() and
 * dwt_setrxantennadelay() instead of the Kconfig defaults.
 *
 * OTP word layout:
 *   bits[15:0]  — PRF 16 MHz delay (irrelevant for this config — PRF 64 in use)
 *   bits[31:16] — PRF 64 MHz delay → extracted and applied to both TX and RX
 *
 * Acceptance criterion (b) from UWB-156.
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_config, test_antenna_delay_otp_overrides_kconfig)
{
    /*
     * Simulate a factory-programmed device:
     *   PRF-64 delay (bits[31:16]) = 0x4040 (16448 ticks — slightly higher
     *   than the Kconfig default 16436, chosen to be unambiguously different)
     *   PRF-16 delay (bits[15:0])  = 0x3E80 (some other value, not used)
     */
    const uint16 otp_prf64_delay = 0x4040u;
    const uint16 otp_prf16_delay = 0x3E80u;

    mock_otp_state.antdly_word = ((uint32)otp_prf64_delay << 16) |
                                 (uint32)otp_prf16_delay;

    (void)dw1000_configure();

    /* Both TX and RX must use the OTP PRF-64 half. */
    zassert_equal(mock_antenna_state.tx_delay, otp_prf64_delay,
        "TX delay: expected OTP PRF-64 value 0x%04X; got 0x%04X",
        (unsigned)otp_prf64_delay, (unsigned)mock_antenna_state.tx_delay);

    zassert_equal(mock_antenna_state.rx_delay, otp_prf64_delay,
        "RX delay: expected OTP PRF-64 value 0x%04X; got 0x%04X",
        (unsigned)otp_prf64_delay, (unsigned)mock_antenna_state.rx_delay);

    /* Confirm the Kconfig default was NOT used (guard against equal values). */
    zassert_not_equal(otp_prf64_delay, (uint16)CONFIG_DW1000_ANTENNA_DELAY_TX,
        "Test misconfigured: OTP value equals Kconfig default — "
        "choose a different OTP test value");
}

/* ---------------------------------------------------------------------------
 * Test 11 — dw1000_set_antenna_delay() overrides at runtime
 *
 * After dw1000_configure() has applied the initial delays, calling
 * dw1000_set_antenna_delay(tx, rx) must:
 *   (a) call dwt_settxantennadelay(tx) and dwt_setrxantennadelay(rx), and
 *   (b) update the cached values returned by dw1000_get_antenna_delay().
 *
 * TX and RX values are overridden independently (different values used here
 * to confirm both registers are written separately).
 *
 * Acceptance criterion (c) from UWB-156.
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_config, test_set_antenna_delay_runtime_override)
{
    /* Start from a known state: configure with OTP empty → Kconfig defaults. */
    (void)dw1000_configure();

    /* Reset call counters so we can isolate the set_antenna_delay() calls. */
    mock_antenna_state.tx_called = 0;
    mock_antenna_state.rx_called = 0;

    /* Override with distinct TX and RX values. */
    const uint16_t new_tx = 0x5000u;
    const uint16_t new_rx = 0x5010u;

    dw1000_set_antenna_delay(new_tx, new_rx);

    /* dwt_settxantennadelay / dwt_setrxantennadelay called once each. */
    zassert_equal(mock_antenna_state.tx_called, 1,
        "Expected dwt_settxantennadelay() called once after set; got %d",
        mock_antenna_state.tx_called);

    zassert_equal(mock_antenna_state.tx_delay, (uint16)new_tx,
        "TX delay after override: expected 0x%04X; got 0x%04X",
        (unsigned)new_tx, (unsigned)mock_antenna_state.tx_delay);

    zassert_equal(mock_antenna_state.rx_called, 1,
        "Expected dwt_setrxantennadelay() called once after set; got %d",
        mock_antenna_state.rx_called);

    zassert_equal(mock_antenna_state.rx_delay, (uint16)new_rx,
        "RX delay after override: expected 0x%04X; got 0x%04X",
        (unsigned)new_rx, (unsigned)mock_antenna_state.rx_delay);

    /* dw1000_get_antenna_delay() must reflect the new values. */
    uint16_t get_tx = 0u;
    uint16_t get_rx = 0u;

    dw1000_get_antenna_delay(&get_tx, &get_rx);

    zassert_equal(get_tx, new_tx,
        "get_antenna_delay TX: expected 0x%04X; got 0x%04X",
        (unsigned)new_tx, (unsigned)get_tx);

    zassert_equal(get_rx, new_rx,
        "get_antenna_delay RX: expected 0x%04X; got 0x%04X",
        (unsigned)new_rx, (unsigned)get_rx);
}
