/*! ----------------------------------------------------------------------------
 * @file    test_join.c
 * @brief   ztest suite: tag-side Aloha join state machine (UWB-261)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI).
 * Compiles the real uwb_join.c against uwb_join_codec.c (UWB-259),
 * dw1000_sync.c (UWB-242), and dw1000_ranging.c (UWB-231) -- see
 * tests/join/CMakeLists.txt.
 *
 * Covers, per the UWB-261 acceptance criteria:
 *   - Network-alive gate: with the gate on and the mock never delivering a
 *     SYNC, the machine still issues a JOIN_REQUEST after the bounded
 *     window (mock TX capture) -- test_gate_bounded_no_sync_still_requests.
 *   - Network-alive gate: an early SYNC satisfies the gate before the
 *     window elapses -- test_gate_sync_heard_transitions_early.
 *   - First-attempt success (mock delivers a matching SLOT_ASSIGNMENT within
 *     the await window) reaches JOINED and exposes short_addr/slot_idx/
 *     slot_count/slot_duration_us exactly as sent --
 *     test_first_attempt_success_reaches_joined.
 *   - On timeout, retries after a delay computed from the injected RNG +
 *     truncated-exponential window; window grows across attempts --
 *     test_timeout_then_backoff_growth_and_retry.
 *   - The backoff window is capped at W_max --
 *     test_backoff_window_capped_at_w_max.
 *   - A SLOT_ASSIGNMENT with target_eui64 != own is ignored (no false
 *     adopt) -- test_foreign_eui64_ignored_no_false_adopt.
 *   - The EUI-64 in JOIN_REQUEST comes from the injected getter, never
 *     hard-coded -- test_eui64_from_injected_getter_not_hardcoded.
 *   - A JOIN_REQUEST TX failure is treated as a failed attempt (same
 *     backoff path as an AWAIT timeout) --
 *     test_request_tx_error_treated_as_failed_attempt.
 *   - An attempt cap triggers a full give-up-and-restart --
 *     test_attempt_cap_gives_up_and_restarts.
 *   - NULL args / missing injected callbacks are rejected without touching
 *     the radio -- test_null_args_rejected_without_touching_radio.
 *   - uwb_join_state_init() enters GATE (gate_cycles > 0) or REQUEST
 *     (gate_cycles == 0) and reads the EUI-64 via the injected getter --
 *     test_state_init_gate_vs_no_gate.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "deca_device_api.h"
#include "deca_regs.h"
#include "dw1000_ranging.h"
#include "mock_deca_driver.h"
#include "uwb_frames.h"
#include "uwb_join.h"
#include "uwb_join_codec.h"

/* ---------------------------------------------------------------------------
 * Suite setup / teardown
 * --------------------------------------------------------------------------- */
static void before_each(void *fixture)
{
    (void)fixture;
    mock_deca_reset();
}

ZTEST_SUITE(uwb_join, NULL, NULL, before_each, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Injected seam stubs
 * --------------------------------------------------------------------------- */

static const uint8_t g_self_eui64[UWB_EUI64_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
};
static const uint8_t g_other_eui64[UWB_EUI64_LEN] = {
    0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22
};

static void stub_eui64_get_self(uint8_t eui64_out[UWB_EUI64_LEN])
{
    memcpy(eui64_out, g_self_eui64, UWB_EUI64_LEN);
}

static void stub_eui64_get_other(uint8_t eui64_out[UWB_EUI64_LEN])
{
    memcpy(eui64_out, g_other_eui64, UWB_EUI64_LEN);
}

/* Captures every stub_rng() call's (min, max) arguments and returns a
 * test-controlled value -- deterministic backoff delays. */
struct rng_capture {
    int      call_count;
    uint32_t last_min;
    uint32_t last_max;
    uint32_t next_value;
};
static struct rng_capture g_rng;

static uint32_t stub_rng(uint32_t min, uint32_t max)
{
    g_rng.call_count++;
    g_rng.last_min = min;
    g_rng.last_max = max;
    return g_rng.next_value;
}

/* Fixed "now" reference -- dw1000_tx_at() is mocked, so no real hardware
 * timing constraint applies; any value inside the 40-bit range exercises the
 * dw1000_delayed_tx_time() computation deterministically. */
static uint64_t stub_now_get(void)
{
    return 0x0000010000000000ULL >> 8; /* an arbitrary 40-bit-range value */
}

static void reset_rng_capture(uint32_t next_value)
{
    memset(&g_rng, 0, sizeof(g_rng));
    g_rng.next_value = next_value;
}

/* ---------------------------------------------------------------------------
 * Default config fixture
 * --------------------------------------------------------------------------- */
static uwb_join_config_t make_default_cfg(void)
{
    uwb_join_config_t cfg = {0};

    cfg.eui64_get = stub_eui64_get_self;
    cfg.rng = stub_rng;
    cfg.now_get = stub_now_get;
    cfg.capabilities = 0;
    cfg.gate_cycles = 0;
    cfg.await_cycles = 3;
    cfg.backoff_base_cycles = 2;
    cfg.backoff_max_cycles = 16;
    cfg.max_attempts = 0; /* unlimited unless a test overrides it */
    cfg.sync_timeout_us = 1000;
    cfg.assignment_timeout_us = 1000;
    cfg.tx_margin_dtu = 1000;

    return cfg;
}

/* ---------------------------------------------------------------------------
 * Test frame builders -- hand-pack raw frame buffers byte-by-byte from the
 * UWB_OFF_* offsets in uwb_frames.h, deliberately independent of
 * uwb_join_codec.c's / dw1000_sync.c's own encode/decode logic (same
 * rationale as tests/dw1000_sync/ and tests/tracking/'s build_sync_frame()).
 * --------------------------------------------------------------------------- */

static void build_sync_frame(uint8_t *buf, uint16_t cycle_seq, uint64_t master_tx_ts)
{
    int i;

    memset(buf, 0, UWB_SYNC_FRAME_SIZE);

    buf[UWB_OFF_FRAME_CTRL]     = UWB_FRAME_CTRL_LOW;
    buf[UWB_OFF_FRAME_CTRL + 1] = UWB_FRAME_CTRL_HIGH;
    buf[UWB_OFF_SEQ_NUM]        = 0x01u;

    buf[UWB_OFF_PAN_ID]     = (uint8_t)(UWB_PAN_ID & 0xFFu);
    buf[UWB_OFF_PAN_ID + 1] = (uint8_t)((UWB_PAN_ID >> 8) & 0xFFu);

    buf[UWB_OFF_DEST_ADDR]     = (uint8_t)(UWB_ADDR_BROADCAST & 0xFFu);
    buf[UWB_OFF_DEST_ADDR + 1] = (uint8_t)((UWB_ADDR_BROADCAST >> 8) & 0xFFu);

    buf[UWB_OFF_SRC_ADDR]     = 0x01u; /* Master's short addr -- unvalidated here. */
    buf[UWB_OFF_SRC_ADDR + 1] = 0x00u;

    buf[UWB_OFF_FRAME_TYPE] = (uint8_t)UWB_FRAME_TYPE_SYNC;

    buf[UWB_OFF_SYNC_CYCLE_SEQ]     = (uint8_t)(cycle_seq & 0xFFu);
    buf[UWB_OFF_SYNC_CYCLE_SEQ + 1] = (uint8_t)((cycle_seq >> 8) & 0xFFu);

    for (i = 0; i < 5; i++) {
        buf[UWB_OFF_SYNC_MASTER_TX_TS + i] = (uint8_t)((master_tx_ts >> (8 * i)) & 0xFFu);
    }
}

static void build_slot_assignment_frame(uint8_t *buf,
                                         const uint8_t target_eui64[UWB_EUI64_LEN],
                                         uint16_t short_addr, uint8_t slot_idx,
                                         uint8_t slot_count, uint16_t slot_duration_us)
{
    memset(buf, 0, UWB_SLOT_ASSIGNMENT_FRAME_SIZE);

    buf[UWB_OFF_FRAME_CTRL]     = UWB_FRAME_CTRL_LOW;
    buf[UWB_OFF_FRAME_CTRL + 1] = UWB_FRAME_CTRL_HIGH;
    buf[UWB_OFF_SEQ_NUM]        = 0x01u;

    buf[UWB_OFF_PAN_ID]     = (uint8_t)(UWB_PAN_ID & 0xFFu);
    buf[UWB_OFF_PAN_ID + 1] = (uint8_t)((UWB_PAN_ID >> 8) & 0xFFu);

    buf[UWB_OFF_DEST_ADDR]     = (uint8_t)(UWB_ADDR_BROADCAST & 0xFFu);
    buf[UWB_OFF_DEST_ADDR + 1] = (uint8_t)((UWB_ADDR_BROADCAST >> 8) & 0xFFu);

    buf[UWB_OFF_SRC_ADDR]     = 0x01u; /* Master's short addr -- unvalidated here. */
    buf[UWB_OFF_SRC_ADDR + 1] = 0x00u;

    buf[UWB_OFF_FRAME_TYPE] = (uint8_t)UWB_FRAME_TYPE_SLOT_ASSIGNMENT;

    memcpy(&buf[UWB_OFF_SLOT_TARGET_EUI64], target_eui64, UWB_EUI64_LEN);

    buf[UWB_OFF_SLOT_SHORT_ADDR]     = (uint8_t)(short_addr & 0xFFu);
    buf[UWB_OFF_SLOT_SHORT_ADDR + 1] = (uint8_t)((short_addr >> 8) & 0xFFu);

    buf[UWB_OFF_SLOT_IDX]   = slot_idx;
    buf[UWB_OFF_SLOT_COUNT] = slot_count;

    buf[UWB_OFF_SLOT_DURATION_US]     = (uint8_t)(slot_duration_us & 0xFFu);
    buf[UWB_OFF_SLOT_DURATION_US + 1] = (uint8_t)((slot_duration_us >> 8) & 0xFFu);
}

/* ---------------------------------------------------------------------------
 * Mock configuration helpers
 * --------------------------------------------------------------------------- */

static void arm_rx_timeout(void)
{
    mock_reg_state.sys_status = SYS_STATUS_RXRFTO;
}

static void arm_sync_heard(uint16_t cycle_seq, uint64_t master_tx_ts)
{
    uint8_t frame[UWB_SYNC_FRAME_SIZE];

    build_sync_frame(frame, cycle_seq, master_tx_ts);

    mock_reg_state.sys_status = SYS_STATUS_RXFCG;
    mock_reg_state.rx_finfo = (uint32)UWB_SYNC_FRAME_SIZE;
    memcpy(mock_readrxdata_state.injected_payload, frame, sizeof(frame));
}

static void arm_slot_assignment_heard(const uint8_t target_eui64[UWB_EUI64_LEN],
                                       uint16_t short_addr, uint8_t slot_idx,
                                       uint8_t slot_count, uint16_t slot_duration_us)
{
    uint8_t frame[UWB_SLOT_ASSIGNMENT_FRAME_SIZE];

    build_slot_assignment_frame(frame, target_eui64, short_addr, slot_idx, slot_count,
                                 slot_duration_us);

    mock_reg_state.sys_status = SYS_STATUS_RXFCG;
    mock_reg_state.rx_finfo = (uint32)UWB_SLOT_ASSIGNMENT_FRAME_SIZE;
    memcpy(mock_readrxdata_state.injected_payload, frame, sizeof(frame));
}

/* Arms the mock so a subsequent dw1000_tx_at() (REQUEST phase) succeeds:
 * dwt_starttx() returns DWT_SUCCESS (mock_deca_reset()'s default) and TXFRS
 * is already latched so the post-starttx completion poll does not spin. */
static void arm_tx_ok(void)
{
    mock_reg_state.sys_status = SYS_STATUS_TXFRS;
}

/* ---------------------------------------------------------------------------
 * Decode helpers over the raw transmitted JOIN_REQUEST buffer
 * --------------------------------------------------------------------------- */

static uint16_t decode_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* =============================================================================
 * Network-alive gate: bounded, never hears a SYNC -> still requests
 * ========================================================================= */
ZTEST(uwb_join, test_gate_bounded_no_sync_still_requests)
{
    uwb_join_config_t cfg = make_default_cfg();
    uwb_join_state_t state;
    int i;

    cfg.gate_cycles = 3;
    reset_rng_capture(0);

    uwb_join_state_init(&state, &cfg);
    zassert_equal(state.phase, UWB_JOIN_PHASE_GATE, "must start in the gate phase");

    for (i = 0; i < 3; i++) {
        arm_rx_timeout();

        uwb_join_step_outcome_t outcome = uwb_join_step(&cfg, &state);

        zassert_equal(outcome, UWB_JOIN_STEP_GATE_LISTENING,
            "gate call %d: expected GATE_LISTENING, got %d", i, (int)outcome);
    }

    zassert_equal(mock_tx_state.writetxdata_called, 0,
        "no JOIN_REQUEST must be transmitted while the gate window is open");
    zassert_equal(state.phase, UWB_JOIN_PHASE_REQUEST,
        "gate window exhausted -- machine must now be ready to request");

    /* One more step: the bounded gate window has elapsed with no SYNC ever
     * heard -- the machine must still issue a JOIN_REQUEST. */
    arm_tx_ok();
    uwb_join_step_outcome_t outcome = uwb_join_step(&cfg, &state);

    zassert_equal(outcome, UWB_JOIN_STEP_REQUEST_SENT,
        "expected REQUEST_SENT after the gate window elapsed; got %d", (int)outcome);
    zassert_equal(mock_tx_state.writetxdata_called, 1,
        "JOIN_REQUEST must have been transmitted exactly once");
    zassert_equal(mock_tx_state.writetxdata_len,
        (uint16)(UWB_JOIN_REQUEST_FRAME_SIZE + 2u),
        "writetxdata length must be the JOIN_REQUEST PSDU size + 2-byte CRC");

    zassert_mem_equal(&mock_tx_state.writetxdata_buf[UWB_OFF_JOIN_EUI64],
        g_self_eui64, UWB_EUI64_LEN,
        "transmitted JOIN_REQUEST EUI-64 must match the injected getter's value");
    zassert_equal(mock_tx_state.writetxdata_buf[UWB_OFF_JOIN_CAPABILITIES], 0,
        "capabilities byte must match cfg.capabilities (0)");
    zassert_equal(decode_le16(&mock_tx_state.writetxdata_buf[UWB_OFF_DEST_ADDR]),
        UWB_ADDR_BROADCAST, "JOIN_REQUEST dest_addr must be the broadcast address");
    zassert_equal(decode_le16(&mock_tx_state.writetxdata_buf[UWB_OFF_SRC_ADDR]),
        UWB_ADDR_UNASSIGNED, "JOIN_REQUEST src_addr must be UWB_ADDR_UNASSIGNED (0xFFFE)");
}

/* =============================================================================
 * Network-alive gate: an early SYNC satisfies the gate before the window
 * elapses
 * ========================================================================= */
ZTEST(uwb_join, test_gate_sync_heard_transitions_early)
{
    uwb_join_config_t cfg = make_default_cfg();
    uwb_join_state_t state;

    cfg.gate_cycles = 5;

    uwb_join_state_init(&state, &cfg);

    arm_sync_heard(0x0001u, 0x0ABCDEULL);
    uwb_join_step_outcome_t outcome = uwb_join_step(&cfg, &state);

    zassert_equal(outcome, UWB_JOIN_STEP_GATE_SYNC_HEARD,
        "expected GATE_SYNC_HEARD on the first call; got %d", (int)outcome);
    zassert_equal(state.phase, UWB_JOIN_PHASE_REQUEST,
        "hearing a SYNC must satisfy the gate immediately, well before the "
        "5-cycle window would otherwise elapse");
}

/* =============================================================================
 * First-attempt success: reaches JOINED and exposes the adopted config
 * exactly as sent
 * ========================================================================= */
ZTEST(uwb_join, test_first_attempt_success_reaches_joined)
{
    uwb_join_config_t cfg = make_default_cfg();
    uwb_join_state_t state;
    uwb_join_result_t result;

    uwb_join_state_init(&state, &cfg);
    zassert_equal(state.phase, UWB_JOIN_PHASE_REQUEST,
        "gate_cycles == 0 must start straight at REQUEST");

    arm_tx_ok();
    zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_REQUEST_SENT,
        "first call must send the JOIN_REQUEST");
    zassert_false(uwb_join_get_result(&state, NULL),
        "must not report joined before a matching assignment arrives");

    arm_slot_assignment_heard(g_self_eui64, 0x1234u, 5u, 24u, 500u);
    uwb_join_step_outcome_t outcome = uwb_join_step(&cfg, &state);

    zassert_equal(outcome, UWB_JOIN_STEP_JOINED,
        "expected JOINED on a matching first-try SLOT_ASSIGNMENT; got %d", (int)outcome);
    zassert_true(uwb_join_get_result(&state, &result),
        "uwb_join_get_result() must report true once joined");
    zassert_equal(result.short_addr, 0x1234u, "short_addr must match the sent value");
    zassert_equal(result.slot_idx, 5u, "slot_idx must match the sent value");
    zassert_equal(result.slot_count, 24u, "slot_count must match the sent value");
    zassert_equal(result.slot_duration_us, 500u,
        "slot_duration_us must match the sent value");

    /* Terminal: a further step() is a no-op. */
    zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_ALREADY_JOINED,
        "a step() call after JOINED must be a no-op");
}

/* =============================================================================
 * Timeout -> backoff (injected RNG) -> retry; window grows across attempts
 * ========================================================================= */
ZTEST(uwb_join, test_timeout_then_backoff_growth_and_retry)
{
    uwb_join_config_t cfg = make_default_cfg();
    uwb_join_state_t state;

    cfg.await_cycles = 1;
    cfg.backoff_base_cycles = 2;
    cfg.backoff_max_cycles = 100;

    uwb_join_state_init(&state, &cfg);

    /* Attempt 0: request, then the 1-cycle await window times out. */
    arm_tx_ok();
    zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_REQUEST_SENT,
        "attempt 0: expected REQUEST_SENT");

    reset_rng_capture(2u); /* rng returns the full window -- deterministic delay */
    arm_rx_timeout();
    uwb_join_step_outcome_t o1 = uwb_join_step(&cfg, &state);

    zassert_equal(o1, UWB_JOIN_STEP_AWAIT_TIMEOUT,
        "attempt 0 timeout: expected AWAIT_TIMEOUT; got %d", (int)o1);
    zassert_equal(g_rng.call_count, 1, "rng must be called exactly once on timeout");
    zassert_equal(g_rng.last_min, 0u, "rng min must be 0");
    zassert_equal(g_rng.last_max, 2u,
        "attempt 0 backoff window must be backoff_base_cycles (2 << 0 = 2)");
    zassert_equal(state.attempt, 1u, "attempt counter must increment to 1");
    zassert_equal(state.phase, UWB_JOIN_PHASE_BACKOFF, "must enter BACKOFF");
    zassert_equal(state.cycles_remaining, 2u,
        "cycles_remaining must be the rng-returned delay (2)");

    /* Count down the 2-cycle backoff. */
    zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_BACKOFF_WAITING,
        "backoff tick 1/2");
    zassert_equal(state.phase, UWB_JOIN_PHASE_BACKOFF, "still backing off");
    zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_BACKOFF_WAITING,
        "backoff tick 2/2");
    zassert_equal(state.phase, UWB_JOIN_PHASE_REQUEST,
        "backoff exhausted -- must be ready to request again");

    /* Attempt 1: a second JOIN_REQUEST, with an incremented MAC sequence. */
    arm_tx_ok();
    zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_REQUEST_SENT,
        "attempt 1: expected REQUEST_SENT");
    zassert_equal(mock_tx_state.writetxdata_buf[UWB_OFF_SEQ_NUM], 1u,
        "second JOIN_REQUEST must carry MAC seq 1 (the first used seq 0)");

    /* Attempt 1 also times out -- the backoff window must have grown
     * (truncated-exponential: base << attempt). */
    reset_rng_capture(0u);
    arm_rx_timeout();
    uwb_join_step_outcome_t o2 = uwb_join_step(&cfg, &state);

    zassert_equal(o2, UWB_JOIN_STEP_AWAIT_TIMEOUT, "attempt 1 timeout");
    zassert_equal(g_rng.last_max, 4u,
        "attempt 1 backoff window must have grown to 2 << 1 = 4");
    zassert_equal(state.attempt, 2u, "attempt counter must increment to 2");
}

/* =============================================================================
 * Backoff window growth is capped at W_max
 * ========================================================================= */
ZTEST(uwb_join, test_backoff_window_capped_at_w_max)
{
    uwb_join_config_t cfg = make_default_cfg();
    uwb_join_state_t state;
    static const uint32_t expected_windows[] = {3u, 5u, 5u, 5u};
    size_t i;

    cfg.await_cycles = 1;
    cfg.backoff_base_cycles = 3;
    cfg.backoff_max_cycles = 5;

    uwb_join_state_init(&state, &cfg);

    for (i = 0; i < sizeof(expected_windows) / sizeof(expected_windows[0]); i++) {
        arm_tx_ok();
        zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_REQUEST_SENT,
            "round %zu: expected REQUEST_SENT", i);

        reset_rng_capture(0u); /* immediate retry -- exercises the window
                                   value itself, not the countdown */
        arm_rx_timeout();
        zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_AWAIT_TIMEOUT,
            "round %zu: expected AWAIT_TIMEOUT", i);

        zassert_equal(g_rng.last_max, expected_windows[i],
            "round %zu: expected backoff window %u, got %u",
            i, (unsigned)expected_windows[i], (unsigned)g_rng.last_max);

        /* delay == 0 -- one BACKOFF_WAITING tick transitions straight back
         * to REQUEST for the next round. */
        zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_BACKOFF_WAITING,
            "round %zu: expected BACKOFF_WAITING", i);
        zassert_equal(state.phase, UWB_JOIN_PHASE_REQUEST,
            "round %zu: zero-delay backoff must transition straight back to "
            "REQUEST", i);
    }
}

/* =============================================================================
 * A SLOT_ASSIGNMENT for a different EUI-64 is ignored -- no false adopt
 * ========================================================================= */
ZTEST(uwb_join, test_foreign_eui64_ignored_no_false_adopt)
{
    uwb_join_config_t cfg = make_default_cfg();
    uwb_join_state_t state;

    cfg.await_cycles = 2;

    uwb_join_state_init(&state, &cfg);

    arm_tx_ok();
    zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_REQUEST_SENT,
        "expected REQUEST_SENT");

    /* A SLOT_ASSIGNMENT addressed to a DIFFERENT tag must be ignored. */
    arm_slot_assignment_heard(g_other_eui64, 0x0099u, 1u, 24u, 500u);
    uwb_join_step_outcome_t outcome = uwb_join_step(&cfg, &state);

    zassert_equal(outcome, UWB_JOIN_STEP_AWAIT_FOREIGN,
        "expected AWAIT_FOREIGN for a non-matching target_eui64; got %d",
        (int)outcome);
    zassert_false(uwb_join_get_result(&state, NULL),
        "must NOT have adopted a foreign assignment");
    zassert_equal(state.phase, UWB_JOIN_PHASE_AWAIT,
        "must remain in AWAIT -- the window is not yet exhausted");

    /* The matching assignment then arrives and IS adopted. */
    arm_slot_assignment_heard(g_self_eui64, 0x00AAu, 2u, 24u, 500u);
    uwb_join_result_t result;

    zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_JOINED,
        "expected JOINED once the correctly-addressed assignment arrives");
    zassert_true(uwb_join_get_result(&state, &result), "must report joined");
    zassert_equal(result.short_addr, 0x00AAu,
        "adopted short_addr must be from the MATCHING assignment, not the "
        "earlier foreign one");
}

/* =============================================================================
 * EUI-64 in JOIN_REQUEST comes from the injected getter -- never hard-coded
 * ========================================================================= */
ZTEST(uwb_join, test_eui64_from_injected_getter_not_hardcoded)
{
    uwb_join_config_t cfg_a = make_default_cfg();
    uwb_join_config_t cfg_b = make_default_cfg();
    uwb_join_state_t state_a;
    uwb_join_state_t state_b;

    cfg_a.eui64_get = stub_eui64_get_self;
    cfg_b.eui64_get = stub_eui64_get_other;

    uwb_join_state_init(&state_a, &cfg_a);
    arm_tx_ok();
    zassert_equal(uwb_join_step(&cfg_a, &state_a), UWB_JOIN_STEP_REQUEST_SENT,
        "getter A: expected REQUEST_SENT");
    zassert_mem_equal(&mock_tx_state.writetxdata_buf[UWB_OFF_JOIN_EUI64],
        g_self_eui64, UWB_EUI64_LEN, "must use getter A's EUI-64");

    uwb_join_state_init(&state_b, &cfg_b);
    arm_tx_ok();
    zassert_equal(uwb_join_step(&cfg_b, &state_b), UWB_JOIN_STEP_REQUEST_SENT,
        "getter B: expected REQUEST_SENT");
    zassert_mem_equal(&mock_tx_state.writetxdata_buf[UWB_OFF_JOIN_EUI64],
        g_other_eui64, UWB_EUI64_LEN,
        "must use getter B's (DIFFERENT) EUI-64 -- confirms it is not "
        "hard-coded anywhere in uwb_join.c");
}

/* =============================================================================
 * A JOIN_REQUEST TX failure is treated as a failed attempt (same backoff
 * path as an AWAIT timeout)
 * ========================================================================= */
ZTEST(uwb_join, test_request_tx_error_treated_as_failed_attempt)
{
    uwb_join_config_t cfg = make_default_cfg();
    uwb_join_state_t state;

    uwb_join_state_init(&state, &cfg);

    reset_rng_capture(1u);
    mock_starttx_state.return_value = DWT_ERROR; /* simulate a missed/HPDWARN slot */

    uwb_join_step_outcome_t outcome = uwb_join_step(&cfg, &state);

    zassert_equal(outcome, UWB_JOIN_STEP_REQUEST_TX_ERROR,
        "expected REQUEST_TX_ERROR; got %d", (int)outcome);
    zassert_equal(g_rng.call_count, 1,
        "a TX failure must still draw a backoff delay, same as a timeout");
    zassert_equal(state.attempt, 1u, "attempt counter must increment");
    zassert_equal(state.phase, UWB_JOIN_PHASE_BACKOFF,
        "must enter backoff, exactly as an AWAIT timeout would");
}

/* =============================================================================
 * Attempt cap -> give-up-and-restart
 * ========================================================================= */
ZTEST(uwb_join, test_attempt_cap_gives_up_and_restarts)
{
    uwb_join_config_t cfg = make_default_cfg();
    uwb_join_state_t state;

    cfg.gate_cycles = 2;
    cfg.await_cycles = 1;
    cfg.max_attempts = 2;

    uwb_join_state_init(&state, &cfg);

    /* Gate window (2 cycles), no SYNC ever heard. */
    arm_rx_timeout();
    zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_GATE_LISTENING, "gate 1/2");
    arm_rx_timeout();
    zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_GATE_LISTENING, "gate 2/2");
    zassert_equal(state.phase, UWB_JOIN_PHASE_REQUEST, "gate must have elapsed");

    /* Attempt 0: request + timeout -> backoff (attempt becomes 1, below the
     * cap of 2). */
    arm_tx_ok();
    zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_REQUEST_SENT, "attempt 0 request");
    reset_rng_capture(0u);
    arm_rx_timeout();
    zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_AWAIT_TIMEOUT, "attempt 0 timeout");
    zassert_equal(state.attempt, 1u, "attempt must be 1 (below the cap)");
    zassert_equal(state.phase, UWB_JOIN_PHASE_BACKOFF, "must be backing off, not restarted yet");

    zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_BACKOFF_WAITING,
        "zero-delay backoff transitions straight to REQUEST");
    zassert_equal(state.phase, UWB_JOIN_PHASE_REQUEST, "ready for attempt 1");

    /* Attempt 1: request + timeout -> attempt becomes 2, hits the cap ->
     * give-up-and-restart. */
    arm_tx_ok();
    zassert_equal(uwb_join_step(&cfg, &state), UWB_JOIN_STEP_REQUEST_SENT, "attempt 1 request");
    reset_rng_capture(0u);
    arm_rx_timeout();
    uwb_join_step_outcome_t outcome = uwb_join_step(&cfg, &state);

    zassert_equal(outcome, UWB_JOIN_STEP_AWAIT_TIMEOUT,
        "the outcome value for the capped attempt is still AWAIT_TIMEOUT "
        "(the give-up-and-restart is a state-side effect, not a distinct "
        "return value)");
    zassert_equal(state.attempt, 0u,
        "hitting the attempt cap must reset the attempt counter to 0");
    zassert_equal(state.phase, UWB_JOIN_PHASE_GATE,
        "hitting the attempt cap must restart from the gate (gate_cycles > 0)");
    zassert_equal(state.cycles_remaining, cfg.gate_cycles,
        "the restarted gate window must be freshly reset");
}

/* =============================================================================
 * NULL args / missing injected callbacks are rejected without touching the
 * radio
 * ========================================================================= */
ZTEST(uwb_join, test_null_args_rejected_without_touching_radio)
{
    uwb_join_config_t cfg = make_default_cfg();
    uwb_join_state_t state;

    uwb_join_state_init(&state, &cfg);

    zassert_equal(uwb_join_step(NULL, &state), UWB_JOIN_STEP_INVALID_ARGS,
        "NULL cfg must be rejected");
    zassert_equal(uwb_join_step(&cfg, NULL), UWB_JOIN_STEP_INVALID_ARGS,
        "NULL state must be rejected");

    uwb_join_config_t bad_cfg = cfg;

    bad_cfg.eui64_get = NULL;
    zassert_equal(uwb_join_step(&bad_cfg, &state), UWB_JOIN_STEP_INVALID_ARGS,
        "NULL eui64_get must be rejected");

    bad_cfg = cfg;
    bad_cfg.rng = NULL;
    zassert_equal(uwb_join_step(&bad_cfg, &state), UWB_JOIN_STEP_INVALID_ARGS,
        "NULL rng must be rejected");

    bad_cfg = cfg;
    bad_cfg.now_get = NULL;
    zassert_equal(uwb_join_step(&bad_cfg, &state), UWB_JOIN_STEP_INVALID_ARGS,
        "NULL now_get must be rejected");

    zassert_equal(mock_rxenable_state.called, 0,
        "no invalid-args call may touch the radio (dw1000_rx() side)");
    zassert_equal(mock_tx_state.writetxdata_called, 0,
        "no invalid-args call may touch the radio (dw1000_tx_at() side)");
}

/* =============================================================================
 * uwb_join_state_init(): GATE vs no-gate, and the EUI-64 getter is used
 * ========================================================================= */
ZTEST(uwb_join, test_state_init_gate_vs_no_gate)
{
    uwb_join_config_t cfg = make_default_cfg();
    uwb_join_state_t state;

    cfg.gate_cycles = 7;
    uwb_join_state_init(&state, &cfg);
    zassert_equal(state.phase, UWB_JOIN_PHASE_GATE, "gate_cycles > 0 must start in GATE");
    zassert_equal(state.cycles_remaining, 7u, "cycles_remaining must be gate_cycles");
    zassert_mem_equal(state.eui64, g_self_eui64, UWB_EUI64_LEN,
        "state.eui64 must be cached from cfg.eui64_get() at init");
    zassert_equal(state.attempt, 0u, "attempt must start at 0");
    zassert_equal(state.mac_seq, 0u, "mac_seq must start at 0");

    cfg.gate_cycles = 0;
    uwb_join_state_init(&state, &cfg);
    zassert_equal(state.phase, UWB_JOIN_PHASE_REQUEST,
        "gate_cycles == 0 must start straight at REQUEST");
}

ZTEST(uwb_join, test_state_init_null_is_noop)
{
    /* Must not crash. */
    uwb_join_state_init(NULL, NULL);

    uwb_join_config_t cfg = make_default_cfg();

    uwb_join_state_init(NULL, &cfg);
}
