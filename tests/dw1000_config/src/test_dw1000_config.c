/*! ----------------------------------------------------------------------------
 * @file    test_dw1000_config.c
 * @brief   ztest suite: DW1000 PHY configuration (UWB-155)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI).
 *
 * Tests in this suite verify that dw1000_configure() drives the deca_driver
 * API with the exact PHY parameter values mandated by contracts/uwb/phy_config.h
 * version 0.6 (pinned at drivers/dw1000/phy_config.h).
 *
 * Mocked functions (in mock_deca_driver.c and mock_port.c):
 *   dwt_initialise()      — captures config arg (DWT_LOADUCODE vs DWT_LOADNONE)
 *   dwt_configure()       — captures full dwt_config_t struct
 *   dwt_configuretxrf()   — captures dwt_txconfig_t struct
 *   reset_DW1000()        — call counter
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
