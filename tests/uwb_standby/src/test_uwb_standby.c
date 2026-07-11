/*! ----------------------------------------------------------------------------
 * @file    test_uwb_standby.c
 * @brief   ztest suite: STANDBY power-state machine (UWB-319)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI).
 *
 * Covers, per the UWB-319 acceptance criteria:
 *   - MONITORING: input->session_active == false enters STANDBY immediately
 *     (dw1000_sleep() called), regardless of the missed count.
 *   - MONITORING: CONFIG_UWB_SYNC_MAX_MISSED consecutive
 *     input->sync_ok == false reports stay MONITORING; one more enters
 *     STANDBY ("enter on N misses").
 *   - MONITORING: an input->sync_ok == true report resets the missed
 *     counter.
 *   - ASLEEP: uwb_standby_step() returns ASLEEP_WAITING (no radio touched)
 *     until CONFIG_UWB_STANDBY_WAKE_CADENCE_MS has elapsed per the injected
 *     clock ("wake-cadence honoured").
 *   - ASLEEP: once the cadence deadline is reached and a SYNC is heard,
 *     exits STANDBY back to MONITORING ("exit on presence").
 *   - ASLEEP: once the cadence deadline is reached and no SYNC is heard,
 *     re-sleeps and stays ASLEEP ("re-sleep on absence").
 *   - ASLEEP: a dw1000_wake() failure re-sleeps without a presence check and
 *     reports UWB_STANDBY_STEP_WAKE_ERROR.
 *   - NULL-argument rejection.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "deca_device_api.h"
#include "deca_regs.h"
#include "mock_deca_driver.h"
#include "mock_port.h"
#include "mock_seq.h"
#include "uwb_frames.h"
#include "uwb_standby.h"

/* ---------------------------------------------------------------------------
 * Fake injected clock
 * --------------------------------------------------------------------------- */
static uint32_t g_fake_now_ms;

static uint32_t fake_now_get(void)
{
    return g_fake_now_ms;
}

/* ---------------------------------------------------------------------------
 * Suite setup / teardown
 * --------------------------------------------------------------------------- */
static uwb_standby_config_t cfg;
static uwb_standby_state_t  state;

static void before_each(void *fixture)
{
    (void)fixture;

    mock_deca_reset();
    mock_port_reset();
    mock_seq_reset();

    g_fake_now_ms = 0;

    cfg.now_get = fake_now_get;
    cfg.sync_timeout_us = 10000;

    uwb_standby_state_init(&state);
}

ZTEST_SUITE(uwb_standby, NULL, NULL, before_each, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Helper: build a raw SYNC frame (deliberately independent of
 * dw1000_sync.c's own encode/decode logic -- mirrors
 * tests/dw1000_sync/src/test_dw1000_sync.c's build_sync_frame()).
 * --------------------------------------------------------------------------- */
static void build_sync_frame(uint8_t *buf)
{
    memset(buf, 0, UWB_SYNC_FRAME_SIZE);

    buf[UWB_OFF_FRAME_CTRL]     = UWB_FRAME_CTRL_LOW;
    buf[UWB_OFF_FRAME_CTRL + 1] = UWB_FRAME_CTRL_HIGH;
    buf[UWB_OFF_SEQ_NUM]        = 0x07u;

    buf[UWB_OFF_PAN_ID]     = (uint8_t)(UWB_PAN_ID & 0xFFu);
    buf[UWB_OFF_PAN_ID + 1] = (uint8_t)((UWB_PAN_ID >> 8) & 0xFFu);

    buf[UWB_OFF_DEST_ADDR]     = (uint8_t)(UWB_ADDR_BROADCAST & 0xFFu);
    buf[UWB_OFF_DEST_ADDR + 1] = (uint8_t)((UWB_ADDR_BROADCAST >> 8) & 0xFFu);

    buf[UWB_OFF_SRC_ADDR]     = 0x01u;
    buf[UWB_OFF_SRC_ADDR + 1] = 0x00u;

    buf[UWB_OFF_FRAME_TYPE] = UWB_FRAME_TYPE_SYNC;

    buf[UWB_OFF_SYNC_CYCLE_SEQ]     = 0x01u;
    buf[UWB_OFF_SYNC_CYCLE_SEQ + 1] = 0x00u;
    /* master_tx_ts left zeroed -- not consulted by this suite. */
}

/** Arm the mocked RX path to deliver a valid SYNC frame on the next
 *  dw1000_rx() call. */
static void mock_arm_sync_present(void)
{
    static uint8_t frame[UWB_SYNC_FRAME_SIZE];

    build_sync_frame(frame);
    mock_reg_state.sys_status = SYS_STATUS_RXFCG;
    mock_reg_state.rx_finfo   = (uint32)UWB_SYNC_FRAME_SIZE;
    memcpy(mock_readrxdata_state.injected_payload, frame, sizeof(frame));
}

/** Arm the mocked RX path to report a hardware RX timeout (no frame heard)
 *  on the next dw1000_rx() call. */
static void mock_arm_sync_absent(void)
{
    mock_reg_state.sys_status = SYS_STATUS_RXRFTO;
}

/* =============================================================================
 * MONITORING -- entry triggers
 * ========================================================================= */

ZTEST(uwb_standby, test_no_active_session_enters_standby_immediately)
{
    uwb_standby_input_t input = { .session_active = false, .sync_ok = false };

    uwb_standby_step_outcome_t ret = uwb_standby_step(&cfg, &state, &input);

    zassert_equal(ret, UWB_STANDBY_STEP_ENTERED,
        "Expected UWB_STANDBY_STEP_ENTERED on 'no active session'; got %d", ret);
    zassert_true(uwb_standby_is_asleep(&state),
        "Expected phase == ASLEEP after entry");
    zassert_equal(mock_sleep_state.entersleep_called, 1,
        "Expected dw1000_sleep() (dwt_entersleep()) to be called on entry; "
        "got %d calls", mock_sleep_state.entersleep_called);
}

ZTEST(uwb_standby, test_sync_ok_reports_stay_monitoring_and_reset_missed)
{
    uwb_standby_input_t miss = { .session_active = true, .sync_ok = false };
    uwb_standby_input_t ok   = { .session_active = true, .sync_ok = true };
    int i;

    /* Rack up some misses, short of the threshold. */
    for (i = 0; i < (int)CONFIG_UWB_SYNC_MAX_MISSED - 1; i++) {
        uwb_standby_step_outcome_t ret = uwb_standby_step(&cfg, &state, &miss);
        zassert_equal(ret, UWB_STANDBY_STEP_MONITORING,
            "Expected MONITORING while under threshold (i=%d); got %d", i, ret);
    }

    /* A good SYNC resets the counter. */
    uwb_standby_step_outcome_t ret = uwb_standby_step(&cfg, &state, &ok);
    zassert_equal(ret, UWB_STANDBY_STEP_MONITORING,
        "Expected MONITORING on sync_ok; got %d", ret);
    zassert_equal(state.missed, 0, "Expected missed counter reset to 0");

    /* Now it takes a full fresh run of misses to enter STANDBY -- confirms
     * the reset actually took effect (not just cosmetic). */
    for (i = 0; i < (int)CONFIG_UWB_SYNC_MAX_MISSED; i++) {
        ret = uwb_standby_step(&cfg, &state, &miss);
        zassert_equal(ret, UWB_STANDBY_STEP_MONITORING,
            "Expected MONITORING at missed=%d (<= max); got %d",
            i + 1, ret);
    }
    zassert_false(uwb_standby_is_asleep(&state),
        "Must still be MONITORING at exactly CONFIG_UWB_SYNC_MAX_MISSED misses");
}

ZTEST(uwb_standby, test_n_plus_one_missed_syncs_enters_standby)
{
    uwb_standby_input_t miss = { .session_active = true, .sync_ok = false };
    int i;

    for (i = 0; i < (int)CONFIG_UWB_SYNC_MAX_MISSED; i++) {
        uwb_standby_step_outcome_t ret = uwb_standby_step(&cfg, &state, &miss);
        zassert_equal(ret, UWB_STANDBY_STEP_MONITORING,
            "Expected MONITORING at missed=%d; got %d", i + 1, ret);
    }

    /* One more than the tolerance. */
    uwb_standby_step_outcome_t ret = uwb_standby_step(&cfg, &state, &miss);

    zassert_equal(ret, UWB_STANDBY_STEP_ENTERED,
        "Expected UWB_STANDBY_STEP_ENTERED after exceeding "
        "CONFIG_UWB_SYNC_MAX_MISSED consecutive misses; got %d", ret);
    zassert_true(uwb_standby_is_asleep(&state), "Expected phase == ASLEEP");
    zassert_equal(mock_sleep_state.entersleep_called, 1,
        "Expected dw1000_sleep() called exactly once on entry");
}

ZTEST(uwb_standby, test_monitoring_rejects_null_input)
{
    uwb_standby_step_outcome_t ret = uwb_standby_step(&cfg, &state, NULL);

    zassert_equal(ret, UWB_STANDBY_STEP_INVALID_ARGS,
        "Expected INVALID_ARGS for NULL input while MONITORING; got %d", ret);
    zassert_equal(mock_sleep_state.entersleep_called, 0,
        "No radio call expected on invalid args");
}

/* =============================================================================
 * ASLEEP -- wake cadence
 * ========================================================================= */

ZTEST(uwb_standby, test_asleep_waits_for_wake_cadence)
{
    uwb_standby_input_t no_session = { .session_active = false, .sync_ok = false };

    (void)uwb_standby_step(&cfg, &state, &no_session);
    zassert_true(uwb_standby_is_asleep(&state), "precondition: must be ASLEEP");

    mock_sleep_state.entersleep_called = 0;  /* isolate the next assertion */

    /* Not yet at the cadence deadline. */
    g_fake_now_ms = (uint32_t)CONFIG_UWB_STANDBY_WAKE_CADENCE_MS - 1u;

    uwb_standby_step_outcome_t ret = uwb_standby_step(&cfg, &state, NULL);

    zassert_equal(ret, UWB_STANDBY_STEP_ASLEEP_WAITING,
        "Expected ASLEEP_WAITING before the cadence deadline; got %d", ret);
    zassert_equal(mock_port_state.wakeup_called, 0,
        "No WAKEUP pulse expected before the cadence deadline");
    zassert_true(uwb_standby_is_asleep(&state), "Must still be ASLEEP");
}

ZTEST(uwb_standby, test_asleep_wakes_at_cadence_deadline)
{
    uwb_standby_input_t no_session = { .session_active = false, .sync_ok = false };

    (void)uwb_standby_step(&cfg, &state, &no_session);

    mock_arm_sync_absent();
    g_fake_now_ms = (uint32_t)CONFIG_UWB_STANDBY_WAKE_CADENCE_MS;

    uwb_standby_step_outcome_t ret = uwb_standby_step(&cfg, &state, NULL);

    zassert_equal(ret, UWB_STANDBY_STEP_WOKE_NO_PRESENCE,
        "Expected WOKE_NO_PRESENCE at the cadence deadline with no SYNC "
        "heard; got %d", ret);
    zassert_equal(mock_port_state.wakeup_called, 1,
        "Expected exactly one WAKEUP pulse at the cadence deadline");
}

/* =============================================================================
 * ASLEEP -- exit on presence / re-sleep on absence
 * ========================================================================= */

ZTEST(uwb_standby, test_asleep_exits_to_monitoring_on_presence)
{
    uwb_standby_input_t no_session = { .session_active = false, .sync_ok = false };

    (void)uwb_standby_step(&cfg, &state, &no_session);

    mock_arm_sync_present();
    g_fake_now_ms = (uint32_t)CONFIG_UWB_STANDBY_WAKE_CADENCE_MS;

    uwb_standby_step_outcome_t ret = uwb_standby_step(&cfg, &state, NULL);

    zassert_equal(ret, UWB_STANDBY_STEP_WOKE_PRESENT,
        "Expected WOKE_PRESENT when a SYNC frame is heard on wake; got %d", ret);
    zassert_false(uwb_standby_is_asleep(&state),
        "Expected phase -> MONITORING on presence");
    zassert_equal(state.missed, 0,
        "Expected the missed counter reset on the MONITORING re-entry");

    /* No re-sleep on the presence path. */
    zassert_equal(mock_sleep_state.entersleep_called, 1,
        "Expected dw1000_sleep() called exactly once (the original STANDBY "
        "entry) -- no re-sleep on the presence-exit path; got %d calls",
        mock_sleep_state.entersleep_called);
}

ZTEST(uwb_standby, test_asleep_resleeps_on_absence_and_stays_asleep)
{
    uwb_standby_input_t no_session = { .session_active = false, .sync_ok = false };

    (void)uwb_standby_step(&cfg, &state, &no_session);
    zassert_equal(mock_sleep_state.entersleep_called, 1,
        "precondition: one dw1000_sleep() call from entry");

    mock_arm_sync_absent();
    g_fake_now_ms = (uint32_t)CONFIG_UWB_STANDBY_WAKE_CADENCE_MS;

    uwb_standby_step_outcome_t ret = uwb_standby_step(&cfg, &state, NULL);

    zassert_equal(ret, UWB_STANDBY_STEP_WOKE_NO_PRESENCE,
        "Expected WOKE_NO_PRESENCE; got %d", ret);
    zassert_true(uwb_standby_is_asleep(&state),
        "Expected phase to stay ASLEEP after a no-presence wake");
    zassert_equal(mock_sleep_state.entersleep_called, 2,
        "Expected a second dw1000_sleep() call (re-sleep) after no presence "
        "was heard; got %d calls", mock_sleep_state.entersleep_called);

    /* The cadence deadline must have been pushed out again -- immediately
     * re-stepping (without advancing g_fake_now_ms further) must wait. */
    ret = uwb_standby_step(&cfg, &state, NULL);
    zassert_equal(ret, UWB_STANDBY_STEP_ASLEEP_WAITING,
        "Expected ASLEEP_WAITING immediately after a re-sleep (cadence "
        "deadline pushed out); got %d", ret);
}

/* =============================================================================
 * ASLEEP -- wake failure
 * ========================================================================= */

ZTEST(uwb_standby, test_wake_failure_resleeps_without_presence_check)
{
    uwb_standby_input_t no_session = { .session_active = false, .sync_ok = false };

    (void)uwb_standby_step(&cfg, &state, &no_session);

    /* Force dw1000_wake()'s reconfigure (dwt_initialise()) to fail. */
    mock_init_state.return_value = DWT_ERROR;
    g_fake_now_ms = (uint32_t)CONFIG_UWB_STANDBY_WAKE_CADENCE_MS;

    uwb_standby_step_outcome_t ret = uwb_standby_step(&cfg, &state, NULL);

    zassert_equal(ret, UWB_STANDBY_STEP_WAKE_ERROR,
        "Expected WAKE_ERROR when dw1000_wake() fails; got %d", ret);
    zassert_true(uwb_standby_is_asleep(&state),
        "Expected phase to stay ASLEEP after a wake failure");

    /* No presence check attempted -- dwt_rxenable() (the RX-arm call) must
     * not have been reached. */
    zassert_equal(mock_rxenable_state.called, 0,
        "No RX attempt expected when the wake itself failed");

    /* A best-effort re-sleep must still have been issued (entry + retry). */
    zassert_equal(mock_sleep_state.entersleep_called, 2,
        "Expected a best-effort re-sleep after the wake failure; got %d calls",
        mock_sleep_state.entersleep_called);
}

/* =============================================================================
 * NULL-argument rejection
 * ========================================================================= */

ZTEST(uwb_standby, test_step_rejects_null_cfg_and_state)
{
    uwb_standby_input_t input = { .session_active = true, .sync_ok = true };

    zassert_equal(uwb_standby_step(NULL, &state, &input),
        UWB_STANDBY_STEP_INVALID_ARGS, "NULL cfg must be rejected");
    zassert_equal(uwb_standby_step(&cfg, NULL, &input),
        UWB_STANDBY_STEP_INVALID_ARGS, "NULL state must be rejected");

    uwb_standby_config_t bad_cfg = { .now_get = NULL, .sync_timeout_us = 0 };
    zassert_equal(uwb_standby_step(&bad_cfg, &state, &input),
        UWB_STANDBY_STEP_INVALID_ARGS, "NULL cfg->now_get must be rejected");
}

ZTEST(uwb_standby, test_is_asleep_rejects_null_state)
{
    zassert_false(uwb_standby_is_asleep(NULL),
        "uwb_standby_is_asleep(NULL) must return false");
}

/* =============================================================================
 * uwb_standby_state_init
 * ========================================================================= */

ZTEST(uwb_standby, test_state_init_starts_monitoring)
{
    uwb_standby_state_t fresh;

    /* Poison the struct first so init() must actually set every field. */
    memset(&fresh, 0xAA, sizeof(fresh));

    uwb_standby_state_init(&fresh);

    zassert_false(uwb_standby_is_asleep(&fresh),
        "A freshly-initialised state must start MONITORING, not ASLEEP");
    zassert_equal(fresh.missed, 0, "missed must start at 0");
}

ZTEST(uwb_standby, test_state_init_ignores_null)
{
    /* Must not crash. */
    uwb_standby_state_init(NULL);
}
