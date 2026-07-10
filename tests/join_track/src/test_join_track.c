/*! ----------------------------------------------------------------------------
 * @file    test_join_track.c
 * @brief   ztest suite: join -> tracking orchestration seam (UWB-263)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI).
 * Compiles the real uwb_join_track.c against uwb_join.c (UWB-261),
 * uwb_join_codec.c (UWB-259), uwb_tracking.c (UWB-252), dw1000_sync.c
 * (UWB-242), dw1000_ranging.c (UWB-231), uwb_cycle_ref.c (UWB-243),
 * uwb_slot_timing.c (UWB-251), and uwb_blink_codec.c (UWB-250) -- see
 * tests/join_track/CMakeLists.txt.
 *
 * Covers, per the UWB-263 acceptance criteria:
 *   - After a matching SLOT_ASSIGNMENT is adopted, the tag transitions from
 *     the join phase to the track phase, and the FIRST blink built by
 *     uwb_tracking.c carries the ADOPTED short_addr at UWB_OFF_SRC_ADDR (not
 *     any hard-coded/injected value) and is scheduled at the ADOPTED
 *     slot_idx's timing (captured dw1000_tx_at() tx_time_dtu, cross-checked
 *     against dw1000_delayed_tx_time() computed the same way uwb_tracking.c
 *     itself does) -- test_join_then_track_adopts_config_for_blink.
 *   - A tag that never receives a matching SLOT_ASSIGNMENT never leaves the
 *     join phase and never calls the blink dw1000_tx_at() (every captured TX
 *     is JOIN_REQUEST-shaped, never TAG_BLINK-shaped) --
 *     test_never_assigned_never_enters_track_phase.
 *   - A SLOT_ASSIGNMENT for a different EUI-64 does not cause a false adopt
 *     (the seam stays in the join phase) -- covered indirectly via
 *     uwb_join.c's own suite (tests/join/); re-exercised at the seam level
 *     for the join->track transition it gates --
 *     test_foreign_assignment_does_not_transition.
 *   - Once tracking, the seam keeps its assignment across transient SYNC
 *     loss (ADR-0039) -- repeated dw1000_sync_rx() misses report
 *     UWB_JOIN_TRACK_NO_VALID_REF, NOT a return to the join phase --
 *     test_sync_loss_after_track_does_not_rejoin.
 *   - NULL cfg/state are rejected without touching the radio --
 *     test_null_args_rejected_without_touching_radio.
 *   - uwb_join_track_state_init() starts in the join phase and
 *     uwb_join_track_is_tracking() reports false --
 *     test_state_init_starts_in_join_phase.
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
#include "uwb_blink_codec.h"
#include "uwb_frames.h"
#include "uwb_join.h"
#include "uwb_join_codec.h"
#include "uwb_join_track.h"

/* ---------------------------------------------------------------------------
 * Suite setup / teardown
 * --------------------------------------------------------------------------- */
static void before_each(void *fixture)
{
    (void)fixture;
    mock_deca_reset();
}

ZTEST_SUITE(uwb_join_track, NULL, NULL, before_each, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Injected seam stubs (join-phase callbacks)
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

static uint32_t stub_rng(uint32_t min, uint32_t max)
{
    /* Deterministic: always the minimum -- backoff delay/window growth
     * itself is exhaustively covered by tests/join/; this suite only needs
     * SOME deterministic value so retries progress quickly. */
    (void)max;
    return min;
}

static uint64_t stub_now_get(void)
{
    return 0x0000010000000000ULL >> 8; /* arbitrary 40-bit-range value */
}

/* ---------------------------------------------------------------------------
 * Default config fixture
 * --------------------------------------------------------------------------- */
static uwb_join_track_config_t make_default_cfg(void)
{
    uwb_join_track_config_t cfg = {0};

    cfg.join.eui64_get = stub_eui64_get_self;
    cfg.join.rng = stub_rng;
    cfg.join.now_get = stub_now_get;
    cfg.join.capabilities = 0;
    cfg.join.gate_cycles = 0; /* skip the gate -- not this seam's concern */
    cfg.join.await_cycles = 2;
    cfg.join.backoff_base_cycles = 1;
    cfg.join.backoff_max_cycles = 4;
    cfg.join.max_attempts = 0; /* unlimited unless a test overrides it */
    cfg.join.sync_timeout_us = 1000;
    cfg.join.assignment_timeout_us = 1000;
    cfg.join.tx_margin_dtu = 1000;

    cfg.tracking_sync_timeout_us = 1000;

    return cfg;
}

/* ---------------------------------------------------------------------------
 * Test frame builders -- hand-pack raw frame buffers byte-by-byte from the
 * UWB_OFF_* offsets in uwb_frames.h, deliberately independent of the
 * codecs'/dw1000_sync.c's own encode/decode logic (same rationale as
 * tests/join/ and tests/tracking/'s identical builders).
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

/* Arms the mock for a successful dw1000_sync_rx() (used once in the track
 * phase) -- also pre-arms SYS_STATUS_TXFRS so a subsequent dw1000_tx_at()
 * (the blink) succeeds by default, exactly as tests/tracking's
 * configure_good_sync_rx() does. */
static void arm_sync_heard(uint16_t cycle_seq, uint64_t master_tx_ts, uint64_t rx_ts)
{
    uint8_t frame[UWB_SYNC_FRAME_SIZE];
    int i;

    build_sync_frame(frame, cycle_seq, master_tx_ts);

    mock_reg_state.sys_status = SYS_STATUS_RXFCG | SYS_STATUS_TXFRS;
    mock_reg_state.rx_finfo = (uint32)UWB_SYNC_FRAME_SIZE;
    memcpy(mock_readrxdata_state.injected_payload, frame, sizeof(frame));

    for (i = 0; i < 5; i++) {
        mock_timestamp_state.rx_ts_bytes[i] = (uint8_t)((rx_ts >> (8 * i)) & 0xFFu);
    }
}

/* Arms the mock so a JOIN_REQUEST TX (join phase) succeeds. */
static void arm_tx_ok(void)
{
    mock_reg_state.sys_status = SYS_STATUS_TXFRS;
}

static uint16_t decode_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* =============================================================================
 * Join -> track: the FIRST blink after adoption carries the ADOPTED
 * short_addr as MAC src and TXes at the ADOPTED slot's computed timing.
 * ========================================================================= */
ZTEST(uwb_join_track, test_join_then_track_adopts_config_for_blink)
{
    uwb_join_track_config_t cfg = make_default_cfg();
    uwb_join_track_state_t state;
    const uint16_t adopted_short_addr = 0x00AAu;
    const uint8_t  adopted_slot_idx = 5u;
    const uint8_t  adopted_slot_count = 24u;
    const uint16_t adopted_slot_duration_us = 500u;
    const uint64_t rx_ts = 0x0100000000ULL;

    uwb_join_track_state_init(&state, &cfg);
    zassert_false(uwb_join_track_is_tracking(&state),
        "must start in the join phase, not tracking");

    /* Step 1: JOIN_REQUEST transmitted. */
    arm_tx_ok();
    uwb_join_track_outcome_t o1 = uwb_join_track_step(&cfg, &state);

    zassert_equal(o1, UWB_JOIN_TRACK_JOIN_REQUEST_SENT,
        "expected JOIN_REQUEST_SENT; got %d", (int)o1);
    zassert_equal(mock_tx_state.writetxdata_called, 1,
        "JOIN_REQUEST must have been transmitted");
    zassert_equal(mock_tx_state.writetxdata_len,
        (uint16)(UWB_JOIN_REQUEST_FRAME_SIZE + 2u),
        "the only TX so far must be JOIN_REQUEST-shaped");

    /* Step 2: a matching SLOT_ASSIGNMENT arrives -- adopt + transition. */
    arm_slot_assignment_heard(g_self_eui64, adopted_short_addr, adopted_slot_idx,
                               adopted_slot_count, adopted_slot_duration_us);
    uwb_join_track_outcome_t o2 = uwb_join_track_step(&cfg, &state);

    zassert_equal(o2, UWB_JOIN_TRACK_JOINED,
        "expected JOINED on a matching assignment; got %d", (int)o2);
    zassert_true(uwb_join_track_is_tracking(&state),
        "must be in the track phase immediately after adopting");
    zassert_equal(mock_tx_state.writetxdata_called, 1,
        "adopting the assignment must NOT itself transmit anything -- still "
        "only the earlier JOIN_REQUEST");

    /* The adopted config must be exactly what the SLOT_ASSIGNMENT carried --
     * NOT any hard-coded/injected placeholder value. */
    zassert_equal(state.tracking_cfg.self_addr, adopted_short_addr,
        "tracking_cfg.self_addr must be the ADOPTED short_addr");
    zassert_equal(state.tracking_cfg.slot_idx, (uint32_t)adopted_slot_idx,
        "tracking_cfg.slot_idx must be the ADOPTED slot_idx");
    zassert_equal(state.tracking_cfg.slot_count, (uint32_t)adopted_slot_count,
        "tracking_cfg.slot_count must be the ADOPTED slot_count");
    zassert_true(state.tracking_cfg.slot_duration_dtu > 0,
        "tracking_cfg.slot_duration_dtu must be derived (non-zero) from the "
        "adopted slot_duration_us");

    /* Step 3: first tracking cycle -- a SYNC is heard, the blink must be
     * built + scheduled using the ADOPTED config. */
    arm_sync_heard(0x0001u, 0x0ABCDEULL, rx_ts);
    uwb_join_track_outcome_t o3 = uwb_join_track_step(&cfg, &state);

    zassert_equal(o3, UWB_JOIN_TRACK_BLINKED,
        "expected BLINKED on the first tracking cycle; got %d", (int)o3);
    zassert_equal(mock_tx_state.writetxdata_called, 1,
        "dwt_writetxdata() call counter is a latch (set to 1, not "
        "incremented) -- still non-zero after the blink TX");
    zassert_equal(mock_tx_state.writetxdata_len,
        (uint16)(UWB_TAG_BLINK_FRAME_SIZE + 2u),
        "the blink TX must be TAG_BLINK-shaped (%u bytes incl. CRC); got %u",
        (unsigned)(UWB_TAG_BLINK_FRAME_SIZE + 2u),
        (unsigned)mock_tx_state.writetxdata_len);

    /* MAC src_addr (UWB_OFF_SRC_ADDR) in the raw transmitted buffer must be
     * the ADOPTED short_addr. */
    zassert_equal(decode_le16(&mock_tx_state.writetxdata_buf[UWB_OFF_SRC_ADDR]),
        adopted_short_addr,
        "blink MAC src_addr (UWB_OFF_SRC_ADDR) must be the ADOPTED short_addr");

    /* Cross-check via the blink codec's own parser too. */
    uint16_t decoded_src_addr = 0xFFFFu;
    int ret = uwb_parse_tag_blink(mock_tx_state.writetxdata_buf,
                                   UWB_TAG_BLINK_FRAME_SIZE, &decoded_src_addr,
                                   NULL, NULL);

    zassert_equal(ret, 0, "built blink frame failed to parse back (%d)", ret);
    zassert_equal(decoded_src_addr, adopted_short_addr,
        "uwb_parse_tag_blink()-decoded src_addr must be the ADOPTED short_addr");

    /* The scheduled TX time must be computed from THIS cycle's sync_rx_ts and
     * the ADOPTED slot_idx / (converted) slot_duration_dtu -- exactly the
     * same formula uwb_slot_tx_time()/uwb_tracking.c itself uses. Comparing
     * against the adopted slot_idx (5), NOT slot 0 or any other value,
     * confirms this is not scheduled off a hard-coded/injected slot. */
    uint64_t expected_tx_time = dw1000_delayed_tx_time(
        rx_ts, (uint64_t)state.tracking_cfg.slot_idx * state.tracking_cfg.slot_duration_dtu);

    zassert_equal(mock_delayedtrxtime_state.called, 1,
        "dwt_setdelayedtrxtime() must have been called for the blink");
    zassert_equal(mock_delayedtrxtime_state.starttime,
        (uint32)(expected_tx_time >> 8),
        "scheduled TX time must match the ADOPTED slot's computed timing: "
        "expected 0x%08X; got 0x%08X",
        (unsigned)(expected_tx_time >> 8),
        (unsigned)mock_delayedtrxtime_state.starttime);
}

/* =============================================================================
 * A tag that never receives a matching SLOT_ASSIGNMENT never leaves the join
 * phase and never calls the blink dw1000_tx_at() -- every captured TX stays
 * JOIN_REQUEST-shaped.
 * ========================================================================= */
ZTEST(uwb_join_track, test_never_assigned_never_enters_track_phase)
{
    uwb_join_track_config_t cfg = make_default_cfg();
    uwb_join_track_state_t state;
    int i;

    cfg.join.await_cycles = 1;
    cfg.join.backoff_base_cycles = 1;
    cfg.join.backoff_max_cycles = 2;
    cfg.join.max_attempts = 0; /* never gives up -- stays retrying forever,
                                   same as a real tag that just never hears a
                                   matching assignment */

    uwb_join_track_state_init(&state, &cfg);

    /* Drive ~20 cycles: every REQUEST TXes ok, every AWAIT times out (no
     * SLOT_ASSIGNMENT ever heard), backoff counts down. Nothing should ever
     * transition phase or attempt a blink.
     *
     * Arm the mock according to which join sub-phase THIS call will drive
     * (read from the exposed join_state.phase before stepping) -- a REQUEST
     * call only calls dw1000_tx_at() (needs SYS_STATUS_TXFRS armed so its
     * post-starttx completion poll does not spin forever), an AWAIT call
     * only calls dw1000_rx() (needs an RX-timeout status), and a BACKOFF
     * call touches the radio at all. Arming the wrong one for whichever call
     * is about to happen would hang the test (dw1000_tx_at()'s completion
     * poll never seeing TXFRS). */
    for (i = 0; i < 20; i++) {
        if (state.join_state.phase == UWB_JOIN_PHASE_REQUEST) {
            arm_tx_ok();
        } else if (state.join_state.phase == UWB_JOIN_PHASE_AWAIT) {
            arm_rx_timeout();
        }
        /* UWB_JOIN_PHASE_BACKOFF: no radio call this step -- nothing to arm. */

        uwb_join_track_outcome_t outcome = uwb_join_track_step(&cfg, &state);

        zassert_true(outcome != UWB_JOIN_TRACK_JOINED &&
                         outcome != UWB_JOIN_TRACK_BLINKED &&
                         outcome != UWB_JOIN_TRACK_NO_VALID_REF &&
                         outcome != UWB_JOIN_TRACK_MISSED_WINDOW &&
                         outcome != UWB_JOIN_TRACK_BUILD_ERROR,
            "cycle %d: must stay a join-phase outcome (never enter track "
            "phase); got %d",
            i, (int)outcome);

        zassert_false(uwb_join_track_is_tracking(&state),
            "cycle %d: must never report tracking", i);

        if (mock_tx_state.writetxdata_called) {
            zassert_equal(mock_tx_state.writetxdata_len,
                (uint16)(UWB_JOIN_REQUEST_FRAME_SIZE + 2u),
                "cycle %d: any TX so far must be JOIN_REQUEST-shaped, never "
                "TAG_BLINK-shaped (the blink dw1000_tx_at() must never be "
                "reached)",
                i);
        }
    }

    zassert_equal(state.phase, UWB_JOIN_TRACK_PHASE_JOIN,
        "must still be in the join phase after 20 cycles with no assignment");
}

/* =============================================================================
 * A SLOT_ASSIGNMENT addressed to a DIFFERENT tag must not cause a false
 * adopt / transition at the seam level.
 * ========================================================================= */
ZTEST(uwb_join_track, test_foreign_assignment_does_not_transition)
{
    uwb_join_track_config_t cfg = make_default_cfg();
    uwb_join_track_state_t state;

    cfg.join.await_cycles = 2;

    uwb_join_track_state_init(&state, &cfg);

    arm_tx_ok();
    zassert_equal(uwb_join_track_step(&cfg, &state), UWB_JOIN_TRACK_JOIN_REQUEST_SENT,
        "expected JOIN_REQUEST_SENT");

    arm_slot_assignment_heard(g_other_eui64, 0x0099u, 1u, 24u, 500u);
    uwb_join_track_outcome_t outcome = uwb_join_track_step(&cfg, &state);

    zassert_equal(outcome, UWB_JOIN_TRACK_JOIN_AWAIT_FOREIGN,
        "expected JOIN_AWAIT_FOREIGN for a non-matching target_eui64; got %d",
        (int)outcome);
    zassert_false(uwb_join_track_is_tracking(&state),
        "must NOT have transitioned on a foreign assignment");
    zassert_equal(state.phase, UWB_JOIN_TRACK_PHASE_JOIN, "must remain in the join phase");
}

/* =============================================================================
 * Once tracking, transient SYNC loss must NOT return the seam to the join
 * phase (ADR-0039: assignment kept across transient SYNC loss).
 * ========================================================================= */
ZTEST(uwb_join_track, test_sync_loss_after_track_does_not_rejoin)
{
    uwb_join_track_config_t cfg = make_default_cfg();
    uwb_join_track_state_t state;
    int i;

    uwb_join_track_state_init(&state, &cfg);

    arm_tx_ok();
    zassert_equal(uwb_join_track_step(&cfg, &state), UWB_JOIN_TRACK_JOIN_REQUEST_SENT,
        "expected JOIN_REQUEST_SENT");

    arm_slot_assignment_heard(g_self_eui64, 0x0055u, 2u, 24u, 500u);
    zassert_equal(uwb_join_track_step(&cfg, &state), UWB_JOIN_TRACK_JOINED,
        "expected JOINED");
    zassert_true(uwb_join_track_is_tracking(&state), "must be tracking now");

    /* Many consecutive SYNC misses -- CONFIG_UWB_SYNC_MAX_MISSED (3, injected
     * by this suite's CMakeLists) plus margin. */
    for (i = 0; i < 10; i++) {
        arm_rx_timeout();
        uwb_join_track_outcome_t outcome = uwb_join_track_step(&cfg, &state);

        zassert_true(outcome == UWB_JOIN_TRACK_NO_VALID_REF ||
                         outcome == UWB_JOIN_TRACK_MISSED_WINDOW,
            "cycle %d: sync loss while tracking must stay a track-phase "
            "outcome (never a join-phase outcome / re-JOINED); got %d",
            i, (int)outcome);
        zassert_true(uwb_join_track_is_tracking(&state),
            "cycle %d: must remain in the track phase across transient "
            "SYNC loss (ADR-0039) -- no automatic re-join",
            i);
    }

    zassert_equal(state.phase, UWB_JOIN_TRACK_PHASE_TRACK,
        "must still be in the track phase after sustained sync loss");
}

/* =============================================================================
 * NULL args are rejected without touching the radio
 * ========================================================================= */
ZTEST(uwb_join_track, test_null_args_rejected_without_touching_radio)
{
    uwb_join_track_config_t cfg = make_default_cfg();
    uwb_join_track_state_t state;

    uwb_join_track_state_init(&state, &cfg);

    zassert_equal(uwb_join_track_step(NULL, &state), UWB_JOIN_TRACK_INVALID_ARGS,
        "NULL cfg must be rejected");
    zassert_equal(uwb_join_track_step(&cfg, NULL), UWB_JOIN_TRACK_INVALID_ARGS,
        "NULL state must be rejected");

    zassert_equal(mock_rxenable_state.called, 0,
        "NULL cfg/state must not touch the radio at all");
    zassert_equal(mock_tx_state.writetxdata_called, 0,
        "NULL cfg/state must not touch the radio at all");
}

/* =============================================================================
 * uwb_join_track_state_init(): starts in the join phase, not tracking
 * ========================================================================= */
ZTEST(uwb_join_track, test_state_init_starts_in_join_phase)
{
    uwb_join_track_config_t cfg = make_default_cfg();
    uwb_join_track_state_t state;

    /* Poison the memory first so the assertions below actually exercise
     * uwb_join_track_state_init(), rather than happening to pass on
     * zero-initialised stack memory. */
    memset(&state, 0xAA, sizeof(state));

    uwb_join_track_state_init(&state, &cfg);

    zassert_equal(state.phase, UWB_JOIN_TRACK_PHASE_JOIN,
        "must start in the join phase");
    zassert_false(uwb_join_track_is_tracking(&state), "must not report tracking yet");
    zassert_mem_equal(state.join_state.eui64, g_self_eui64, UWB_EUI64_LEN,
        "join_state.eui64 must be cached from cfg.join.eui64_get() at init");
}

ZTEST(uwb_join_track, test_state_init_null_is_noop)
{
    /* Must not crash. */
    uwb_join_track_state_init(NULL, NULL);

    uwb_join_track_config_t cfg = make_default_cfg();

    uwb_join_track_state_init(NULL, &cfg);
    zassert_false(uwb_join_track_is_tracking(NULL),
        "uwb_join_track_is_tracking(NULL) must report false, not crash");
}
