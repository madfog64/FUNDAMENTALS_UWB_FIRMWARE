/*! ----------------------------------------------------------------------------
 * @file    test_dw1000_sleep.c
 * @brief   ztest suite: DW1000 driver sleep/wake seam (UWB-316)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI/GPIO).
 *
 * Verifies dw1000_sleep()/dw1000_wake() drive the deca_driver + port mocks
 * with the correct arguments and call ORDER — this is the mechanism the
 * STANDBY state machine (UWB-319) will drive, so the call sequence contract
 * is the thing worth locking down here.
 *
 * Mocked functions (mock_deca_driver.c, mock_port.c):
 *   dwt_configuresleep()       — captures mode/wake args + call-order seq
 *   dwt_entersleep()           — captures call-order seq
 *   port_wakeup_dw1000()       — captures call-order seq (WAKEUP pin toggle)
 *   dwt_initialise()           — captures config arg + call-order seq
 *                                 (this is dw1000_configure()'s "device-id
 *                                 re-read" step)
 *   dwt_configure()            — captures call-order seq
 *   reset_DW1000(), port_set_dw1000_slowrate/fastrate(), dwt_configuretxrf(),
 *   dwt_settxantennadelay/dwt_setrxantennadelay(), dwt_otpread() — the rest
 *   of dw1000_configure()'s reconfigure-on-wake path (already covered in
 *   detail by tests/dw1000_config/; only touched here for call-order and
 *   pass/fail propagation).
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <zephyr/ztest.h>

#include "deca_device_api.h"
#include "dw1000_config.h"
#include "dw1000_sleep.h"
#include "mock_deca_driver.h"
#include "mock_port.h"
#include "mock_seq.h"

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
    mock_seq_reset();
}

ZTEST_SUITE(dw1000_sleep, NULL, suite_setup, before_each, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Test 1 — dw1000_sleep() calls dwt_configuresleep() then dwt_entersleep()
 *
 * Both must be called exactly once, and dwt_configuresleep() must be called
 * BEFORE dwt_entersleep() (deca_device_api.h contract: "dwt_configuresleep()
 * should be called first").
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_sleep, test_sleep_calls_configuresleep_then_entersleep)
{
    dw1000_sleep_result_t ret = dw1000_sleep();

    zassert_equal(ret, DW1000_SLEEP_OK,
        "dw1000_sleep() returned %d (expected DW1000_SLEEP_OK)", ret);

    zassert_equal(mock_sleep_state.configuresleep_called, 1,
        "Expected dwt_configuresleep() to be called exactly once; got %d",
        mock_sleep_state.configuresleep_called);

    zassert_equal(mock_sleep_state.entersleep_called, 1,
        "Expected dwt_entersleep() to be called exactly once; got %d",
        mock_sleep_state.entersleep_called);

    zassert_true(mock_sleep_state.configuresleep_seq < mock_sleep_state.entersleep_seq,
        "Expected dwt_configuresleep() (seq=%d) to be called before "
        "dwt_entersleep() (seq=%d)",
        mock_sleep_state.configuresleep_seq, mock_sleep_state.entersleep_seq);
}

/* ---------------------------------------------------------------------------
 * Test 2 — dw1000_sleep() programs the DEEPSLEEP wake trigger correctly
 *
 * mode must request the AON-preserved config to be reloaded on wake
 * (DWT_PRESRV_SLEEP | DWT_CONFIG); wake must enable sleep and request the
 * WAKEUP-pin trigger (DWT_SLP_EN | DWT_WAKE_WK), the mechanism UWB-316 wires.
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_sleep, test_sleep_configures_wake_on_wakeup_pin)
{
    (void)dw1000_sleep();

    zassert_true((mock_sleep_state.configuresleep_mode & DWT_CONFIG) != 0,
        "Expected dwt_configuresleep() mode to include DWT_CONFIG (reload "
        "AON config on wake); got 0x%04X",
        (unsigned)mock_sleep_state.configuresleep_mode);

    zassert_true((mock_sleep_state.configuresleep_mode & DWT_PRESRV_SLEEP) != 0,
        "Expected dwt_configuresleep() mode to include DWT_PRESRV_SLEEP; "
        "got 0x%04X", (unsigned)mock_sleep_state.configuresleep_mode);

    zassert_true((mock_sleep_state.configuresleep_wake & DWT_WAKE_WK) != 0,
        "Expected dwt_configuresleep() wake to include DWT_WAKE_WK (WAKEUP "
        "pin trigger); got 0x%02X",
        (unsigned)mock_sleep_state.configuresleep_wake);

    zassert_true((mock_sleep_state.configuresleep_wake & DWT_SLP_EN) != 0,
        "Expected dwt_configuresleep() wake to include DWT_SLP_EN (sleep "
        "functionality enabled); got 0x%02X",
        (unsigned)mock_sleep_state.configuresleep_wake);
}

/* ---------------------------------------------------------------------------
 * Test 3 — dw1000_wake() toggles WAKEUP, then reconfigures
 *
 * Full order: port_wakeup_dw1000() < reset_DW1000() < dwt_initialise()
 * < dwt_configure() — the WAKEUP toggle must happen before ANY part of the
 * dw1000_configure() reconfigure-on-wake path, and dwt_initialise() (the
 * device-id re-read) must happen before dwt_configure().
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_sleep, test_wake_toggles_wakeup_before_reconfigure)
{
    dw1000_sleep_result_t ret = dw1000_wake();

    zassert_equal(ret, DW1000_SLEEP_OK,
        "dw1000_wake() returned %d (expected DW1000_SLEEP_OK)", ret);

    zassert_equal(mock_port_state.wakeup_called, 1,
        "Expected port_wakeup_dw1000() to be called exactly once; got %d",
        mock_port_state.wakeup_called);

    zassert_equal(mock_init_state.called, 1,
        "Expected dwt_initialise() (device-id re-read) to be called exactly "
        "once; got %d", mock_init_state.called);

    zassert_equal(mock_cfg_state.called, 1,
        "Expected dwt_configure() to be called exactly once; got %d",
        mock_cfg_state.called);

    zassert_true(mock_port_state.wakeup_seq < mock_port_state.reset_seq,
        "Expected port_wakeup_dw1000() (seq=%d) before reset_DW1000() "
        "(seq=%d)", mock_port_state.wakeup_seq, mock_port_state.reset_seq);

    zassert_true(mock_port_state.reset_seq < mock_init_state.seq,
        "Expected reset_DW1000() (seq=%d) before dwt_initialise() (seq=%d)",
        mock_port_state.reset_seq, mock_init_state.seq);

    zassert_true(mock_init_state.seq < mock_cfg_state.seq,
        "Expected dwt_initialise() (seq=%d) before dwt_configure() (seq=%d)",
        mock_init_state.seq, mock_cfg_state.seq);
}

/* ---------------------------------------------------------------------------
 * Test 4 — dw1000_wake() reconfigures with DWT_LOADUCODE
 *
 * The reconfigure-on-wake path must load the LDE microcode, exactly like a
 * fresh dw1000_configure() bring-up (UWB-155) — deep sleep does not preserve
 * the LDE state.
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_sleep, test_wake_reconfigure_uses_loaducode)
{
    (void)dw1000_wake();

    zassert_equal(mock_init_state.config_arg, (uint16)DWT_LOADUCODE,
        "Expected dwt_initialise(DWT_LOADUCODE=0x%04X) during wake "
        "reconfigure; got dwt_initialise(0x%04X)",
        (unsigned)DWT_LOADUCODE, (unsigned)mock_init_state.config_arg);
}

/* ---------------------------------------------------------------------------
 * Test 5 — dw1000_wake() propagates a reconfigure failure
 *
 * If dwt_initialise() fails after the WAKEUP pulse (device did not actually
 * wake / SPI fault), dw1000_wake() must report
 * DW1000_SLEEP_ERR_WAKE_RECONFIGURE — the caller must not proceed to treat
 * the radio as usable.
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_sleep, test_wake_reports_reconfigure_failure)
{
    mock_init_state.return_value = DWT_ERROR;

    dw1000_sleep_result_t ret = dw1000_wake();

    zassert_equal(ret, DW1000_SLEEP_ERR_WAKE_RECONFIGURE,
        "Expected DW1000_SLEEP_ERR_WAKE_RECONFIGURE when dwt_initialise() "
        "fails during wake reconfigure; got %d", ret);

    /* The WAKEUP pulse must still have happened before the failure. */
    zassert_equal(mock_port_state.wakeup_called, 1,
        "Expected port_wakeup_dw1000() to still be called once even though "
        "the reconfigure ultimately failed; got %d",
        mock_port_state.wakeup_called);

    /* dwt_configure() must NOT be reached — dw1000_configure() bails out on
     * dwt_initialise() failure before that step (same contract as UWB-155). */
    zassert_equal(mock_cfg_state.called, 0,
        "dwt_configure() must not be called when the wake reconfigure's "
        "dwt_initialise() fails; was called %d times",
        mock_cfg_state.called);
}

/* ---------------------------------------------------------------------------
 * Test 6 — full sleep -> wake round trip
 *
 * Exercises the seam end-to-end: sleep, then wake, confirming both halves
 * report success independently and the wake path still runs to completion
 * after a prior sleep call (no leftover mock/module state trips it up).
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_sleep, test_sleep_then_wake_round_trip)
{
    dw1000_sleep_result_t sleep_ret = dw1000_sleep();
    dw1000_sleep_result_t wake_ret  = dw1000_wake();

    zassert_equal(sleep_ret, DW1000_SLEEP_OK,
        "dw1000_sleep() returned %d (expected DW1000_SLEEP_OK)", sleep_ret);
    zassert_equal(wake_ret, DW1000_SLEEP_OK,
        "dw1000_wake() returned %d (expected DW1000_SLEEP_OK)", wake_ret);

    zassert_equal(mock_sleep_state.entersleep_called, 1,
        "Expected dwt_entersleep() called once across the round trip; got %d",
        mock_sleep_state.entersleep_called);
    zassert_equal(mock_port_state.wakeup_called, 1,
        "Expected port_wakeup_dw1000() called once across the round trip; "
        "got %d", mock_port_state.wakeup_called);

    /* The whole sleep call must precede the whole wake call. */
    zassert_true(mock_sleep_state.entersleep_seq < mock_port_state.wakeup_seq,
        "Expected dwt_entersleep() (seq=%d) before port_wakeup_dw1000() "
        "(seq=%d)", mock_sleep_state.entersleep_seq, mock_port_state.wakeup_seq);
}
