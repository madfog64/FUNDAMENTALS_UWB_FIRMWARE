/*! ----------------------------------------------------------------------------
 * @file    test_tracking.c
 * @brief   ztest suite: tag-side per-cycle TDoA tracking (blinker) loop
 *          (UWB-252)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI).
 * Compiles the real uwb_tracking.c against dw1000_sync.c (UWB-242),
 * dw1000_ranging.c (UWB-231), uwb_cycle_ref.c (UWB-243), uwb_slot_timing.c
 * (UWB-251), and uwb_blink_codec.c (UWB-250) -- see tests/tracking/CMakeLists.txt.
 *
 * Covers, per the UWB-252 acceptance criteria:
 *   - Never synced -> UWB_TRACKING_NO_VALID_REF, dw1000_tx_at() never called
 *     (mock-verified: dwt_setdelayedtrxtime()/dwt_writetxdata()/dwt_starttx()
 *     all uncalled).
 *   - A cycle after a successful dw1000_sync_rx() builds a correct blink
 *     (parsed back via uwb_parse_tag_blink()) and schedules it at the
 *     slot-timing unit's computed time (mock captures tx_time_dtu + buffer).
 *   - blink_count increments once per actually-transmitted blink, carried LE
 *     in the payload (verified across two consecutive successful cycles).
 *   - After enough consecutive SYNC misses to cross
 *     CONFIG_UWB_SYNC_MAX_MISSED (uwb_cycle_ref_on_miss() drives valid=false
 *     per UWB-243's rule), the loop stops blinking until a fresh SYNC
 *     re-validates.
 *   - A dw1000_tx_at() -EIO (simulated late TX / HPDWARN) is handled without
 *     hanging/retrying, surfaced as UWB_TRACKING_MISSED_WINDOW, and does NOT
 *     increment blink_count.
 *   - An out-of-range slot config (uwb_slot_tx_time() rejects it) surfaces as
 *     UWB_TRACKING_BUILD_ERROR without ever attempting a TX.
 *   - NULL cfg/state are rejected without touching the radio.
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
#include "uwb_cycle_ref.h"
#include "uwb_frames.h"
#include "uwb_tracking.h"

/* ---------------------------------------------------------------------------
 * Suite setup / teardown
 * --------------------------------------------------------------------------- */
static void before_each(void *fixture)
{
    (void)fixture;
    mock_deca_reset();
}

ZTEST_SUITE(uwb_tracking, NULL, NULL, before_each, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Shared test fixture config: 8-slot superframe, this tag in slot 2.
 * --------------------------------------------------------------------------- */
static const uwb_tracking_config_t default_cfg = {
    .self_addr = 0x0042u,
    .slot_idx = 2u,
    .slot_count = 8u,
    .slot_duration_dtu = 32000000u, /* ~0.5 ms superframe slot, matches
                                        tests/slot_timing's fixture scale */
};

/* ---------------------------------------------------------------------------
 * Test frame builder -- hand-packs a raw SYNC frame buffer byte-by-byte from
 * the UWB_OFF_SYNC_* offsets in uwb_frames.h, deliberately independent of
 * dw1000_sync.c's own encode/decode logic (same rationale as
 * tests/dw1000_sync/src/test_dw1000_sync.c's build_sync_frame()).
 * --------------------------------------------------------------------------- */
static void build_sync_frame(uint8_t *buf, uint16_t cycle_seq,
                              uint64_t master_tx_ts)
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

    /* Master's short address -- arbitrary, not validated by uwb_sync_parse(). */
    buf[UWB_OFF_SRC_ADDR]     = 0x01u;
    buf[UWB_OFF_SRC_ADDR + 1] = 0x00u;

    buf[UWB_OFF_FRAME_TYPE] = (uint8_t)UWB_FRAME_TYPE_SYNC;

    buf[UWB_OFF_SYNC_CYCLE_SEQ]     = (uint8_t)(cycle_seq & 0xFFu);
    buf[UWB_OFF_SYNC_CYCLE_SEQ + 1] = (uint8_t)((cycle_seq >> 8) & 0xFFu);

    for (i = 0; i < 5; i++) {
        buf[UWB_OFF_SYNC_MASTER_TX_TS + i] =
            (uint8_t)((master_tx_ts >> (8 * i)) & 0xFFu);
    }
}

/* ---------------------------------------------------------------------------
 * Mock configuration helpers
 * --------------------------------------------------------------------------- */

/*
 * Arms the mock for a successful dw1000_sync_rx() returning a valid SYNC
 * frame with the given cycle_seq/master_tx_ts/rx_ts, AND (since
 * dw1000_tx_at()'s SYS_STATUS_TXFRS poll is independent of the RX status
 * bits -- see dw1000_ranging.c) pre-arms SYS_STATUS_TXFRS so a subsequent
 * dw1000_tx_at() call in the same cycle succeeds by default. A test that
 * wants the TX to fail overrides mock_starttx_state.return_value afterwards.
 */
static void configure_good_sync_rx(uint16_t cycle_seq, uint64_t master_tx_ts,
                                    uint64_t rx_ts)
{
    uint8_t frame[UWB_SYNC_FRAME_SIZE];
    int i;

    build_sync_frame(frame, cycle_seq, master_tx_ts);

    mock_reg_state.sys_status = SYS_STATUS_RXFCG | SYS_STATUS_TXFRS;
    mock_reg_state.rx_finfo   = (uint32)UWB_SYNC_FRAME_SIZE;
    memcpy(mock_readrxdata_state.injected_payload, frame, sizeof(frame));

    for (i = 0; i < 5; i++) {
        mock_timestamp_state.rx_ts_bytes[i] = (uint8_t)((rx_ts >> (8 * i)) & 0xFFu);
    }
}

/*
 * Arms the mock so dw1000_sync_rx() (and hence dw1000_rx()) times out.
 *
 * Also pre-arms SYS_STATUS_TXFRS (same rationale as
 * configure_good_sync_rx() above): when the cycle reference is still within
 * its missed-SYNC tolerance (uwb_cycle_ref.h), uwb_tracking_run_cycle() still
 * attempts a TX this cycle even though no SYNC was heard -- without TXFRS
 * pre-armed, dw1000_tx_at()'s post-dwt_starttx() completion poll would spin
 * forever waiting for a bit the mock never sets. SYS_STATUS_RXRFTO (0x20000,
 * part of SYS_STATUS_ALL_RX_TO) and SYS_STATUS_TXFRS (0x80) do not overlap,
 * so dw1000_rx()'s write-1-to-clear of SYS_STATUS_ALL_RX_TO on the timeout
 * path never touches the TXFRS bit.
 */
static void configure_sync_timeout(void)
{
    mock_reg_state.sys_status = SYS_STATUS_RXRFTO | SYS_STATUS_TXFRS;
}

/* =============================================================================
 * Never synced -> UWB_TRACKING_NO_VALID_REF, no TX attempted whatsoever
 * ========================================================================= */
ZTEST(uwb_tracking, test_never_synced_does_not_transmit)
{
    uwb_tracking_state_t state;

    uwb_tracking_state_init(&state);
    configure_sync_timeout();

    uwb_tracking_outcome_t outcome =
        uwb_tracking_run_cycle(&default_cfg, &state, 1000);

    zassert_equal(outcome, UWB_TRACKING_NO_VALID_REF,
        "expected UWB_TRACKING_NO_VALID_REF; got %d", (int)outcome);
    zassert_equal(mock_delayedtrxtime_state.called, 0,
        "dwt_setdelayedtrxtime() must not be called without a valid ref");
    zassert_equal(mock_tx_state.writetxdata_called, 0,
        "dwt_writetxdata() must not be called without a valid ref");
    zassert_equal(mock_starttx_state.called, 0,
        "dwt_starttx() must not be called without a valid ref");
    zassert_equal(state.blink_count, 0, "blink_count must stay at 0");
}

/* =============================================================================
 * Successful SYNC -> builds a correct blink + schedules it at the
 * slot-timing unit's computed time
 * ========================================================================= */
ZTEST(uwb_tracking, test_valid_sync_builds_and_schedules_blink)
{
    uwb_tracking_state_t state;
    const uint64_t rx_ts = 0x0100000000ULL;

    uwb_tracking_state_init(&state);
    configure_good_sync_rx(0x0001u, 0x0ABCDEULL, rx_ts);

    uwb_tracking_outcome_t outcome =
        uwb_tracking_run_cycle(&default_cfg, &state, 1000);

    zassert_equal(outcome, UWB_TRACKING_BLINKED,
        "expected UWB_TRACKING_BLINKED; got %d", (int)outcome);

    /* Expected TX time computed the same way uwb_slot_tx_time() itself does
     * (tests/slot_timing already covers that unit in isolation -- here we
     * confirm uwb_tracking.c actually calls it with the reference's
     * sync_rx_ts and this tag's slot config, not some other value). */
    uint64_t expected_tx_time = dw1000_delayed_tx_time(
        rx_ts, default_cfg.slot_idx * default_cfg.slot_duration_dtu);

    zassert_equal(mock_delayedtrxtime_state.called, 1,
        "expected dwt_setdelayedtrxtime() called once; got %d",
        mock_delayedtrxtime_state.called);
    zassert_equal(mock_delayedtrxtime_state.starttime,
        (uint32)(expected_tx_time >> 8),
        "dwt_setdelayedtrxtime(): expected high 32 bits of the slot TX time "
        "(0x%08X); got 0x%08X",
        (unsigned)(expected_tx_time >> 8),
        (unsigned)mock_delayedtrxtime_state.starttime);

    zassert_equal(mock_tx_state.writetxdata_called, 1,
        "expected dwt_writetxdata() called once; got %d",
        mock_tx_state.writetxdata_called);
    zassert_equal(mock_tx_state.writetxdata_len,
        (uint16)(UWB_TAG_BLINK_FRAME_SIZE + 2u),
        "writetxdata length must be the blink PSDU size + 2-byte CRC: "
        "expected %u; got %u",
        (unsigned)(UWB_TAG_BLINK_FRAME_SIZE + 2u),
        (unsigned)mock_tx_state.writetxdata_len);

    uint16_t decoded_src_addr = 0;
    uint16_t decoded_blink_count = 0xFFFFu;
    uint8_t decoded_flags = 0xFFu;

    int ret = uwb_parse_tag_blink(mock_tx_state.writetxdata_buf,
                                   UWB_TAG_BLINK_FRAME_SIZE, &decoded_src_addr,
                                   &decoded_blink_count, &decoded_flags);

    zassert_equal(ret, 0, "built blink frame failed to parse back (%d)", ret);
    zassert_equal(decoded_src_addr, default_cfg.self_addr,
        "blink src_addr must be this tag's configured short address");
    zassert_equal(decoded_blink_count, 0,
        "the first transmitted blink must carry blink_count 0");
    zassert_equal(decoded_flags, 0, "no status flags are set by this module");

    zassert_equal(mock_starttx_state.called, 1,
        "expected dwt_starttx() called once; got %d", mock_starttx_state.called);
    zassert_true((mock_starttx_state.mode & (uint8_t)DWT_START_TX_DELAYED) != 0,
        "dwt_starttx() mode must include DWT_START_TX_DELAYED");
    zassert_true((mock_starttx_state.mode & (uint8_t)DWT_RESPONSE_EXPECTED) == 0,
        "a tag blink does not expect a response (dw1000_tx_at expect_response=false)");

    zassert_equal(state.blink_count, 1,
        "blink_count must increment after an actually-transmitted blink");
}

/* =============================================================================
 * blink_count increments once per actually-transmitted blink, carried LE in
 * the payload (checked across two consecutive successful cycles)
 * ========================================================================= */
ZTEST(uwb_tracking, test_blink_count_increments_once_per_transmitted_blink)
{
    uwb_tracking_state_t state;

    uwb_tracking_state_init(&state);

    configure_good_sync_rx(0x0001u, 0x01ULL, 0x1000ULL);
    uwb_tracking_outcome_t o1 = uwb_tracking_run_cycle(&default_cfg, &state, 1000);

    zassert_equal(o1, UWB_TRACKING_BLINKED, "cycle 1: expected BLINKED, got %d", (int)o1);
    zassert_equal(state.blink_count, 1, "cycle 1: blink_count expected 1");

    /* Second cycle: a fresh SYNC (cycle_seq advances by 1, as a real master
     * would), independent good TX. */
    configure_good_sync_rx(0x0002u, 0x02ULL, 0x2000ULL);
    uwb_tracking_outcome_t o2 = uwb_tracking_run_cycle(&default_cfg, &state, 1000);

    zassert_equal(o2, UWB_TRACKING_BLINKED, "cycle 2: expected BLINKED, got %d", (int)o2);

    uint16_t decoded_blink_count = 0xFFFFu;
    int ret = uwb_parse_tag_blink(mock_tx_state.writetxdata_buf,
                                   UWB_TAG_BLINK_FRAME_SIZE, NULL,
                                   &decoded_blink_count, NULL);

    zassert_equal(ret, 0, "second blink frame failed to parse back (%d)", ret);
    zassert_equal(decoded_blink_count, 1,
        "the second transmitted blink must carry blink_count 1 (the value "
        "in effect before this cycle's increment)");
    zassert_equal(state.blink_count, 2, "cycle 2: blink_count expected 2");
}

/* =============================================================================
 * Enough consecutive SYNC misses cross CONFIG_UWB_SYNC_MAX_MISSED -> the
 * loop stops blinking until a fresh SYNC re-validates
 * ========================================================================= */
ZTEST(uwb_tracking, test_sync_loss_stops_blinking_until_resync)
{
    uwb_tracking_state_t state;
    int i;

    uwb_tracking_state_init(&state);

    /* Cycle 0: valid sync, blinks -- establishes a reference to go stale. */
    configure_good_sync_rx(0x0001u, 0x01ULL, 0x1000ULL);
    zassert_equal(uwb_tracking_run_cycle(&default_cfg, &state, 1000),
        UWB_TRACKING_BLINKED, "initial sync must blink");

    /* Up to CONFIG_UWB_SYNC_MAX_MISSED consecutive misses are still within
     * uwb_cycle_ref's tolerance (missed <= MAX_MISSED keeps valid == true,
     * see uwb_cycle_ref.c) -- the tag keeps blinking off the last-known
     * (increasingly stale) reference during this window, by design. */
    for (i = 1; i <= CONFIG_UWB_SYNC_MAX_MISSED; i++) {
        configure_sync_timeout();
        uwb_tracking_outcome_t o = uwb_tracking_run_cycle(&default_cfg, &state, 1000);

        zassert_equal(o, UWB_TRACKING_BLINKED,
            "miss #%d (within CONFIG_UWB_SYNC_MAX_MISSED tolerance) must "
            "still blink off the stale reference; got %d", i, (int)o);
    }

    /* One more miss crosses the tolerance threshold (missed >
     * CONFIG_UWB_SYNC_MAX_MISSED) -> the reference is invalidated and the
     * loop MUST stop blinking. */
    configure_sync_timeout();
    uwb_tracking_outcome_t over_threshold =
        uwb_tracking_run_cycle(&default_cfg, &state, 1000);

    zassert_equal(over_threshold, UWB_TRACKING_NO_VALID_REF,
        "expected UWB_TRACKING_NO_VALID_REF once missed exceeds "
        "CONFIG_UWB_SYNC_MAX_MISSED; got %d", (int)over_threshold);

    /* Stays out of tracking with no fresh SYNC. */
    configure_sync_timeout();
    zassert_equal(uwb_tracking_run_cycle(&default_cfg, &state, 1000),
        UWB_TRACKING_NO_VALID_REF,
        "must remain out of tracking while sync stays lost");

    /* A fresh SYNC re-validates and tracking resumes. */
    configure_good_sync_rx(0x0099u, 0x03ULL, 0x9000ULL);
    zassert_equal(uwb_tracking_run_cycle(&default_cfg, &state, 1000),
        UWB_TRACKING_BLINKED,
        "a fresh SYNC frame must re-validate the reference and resume blinking");
}

/* =============================================================================
 * dw1000_tx_at() -EIO (simulated late TX / HPDWARN) -> handled without
 * hanging/retrying, surfaced as UWB_TRACKING_MISSED_WINDOW, no blink_count
 * increment
 * ========================================================================= */
ZTEST(uwb_tracking, test_tx_missed_window_no_retry_no_increment)
{
    uwb_tracking_state_t state;

    uwb_tracking_state_init(&state);
    configure_good_sync_rx(0x0001u, 0x01ULL, 0x1000ULL);

    /* Simulate a missed scheduled slot: dwt_starttx() reports DWT_ERROR
     * (HPDWARN latched) rather than DWT_SUCCESS. */
    mock_starttx_state.return_value = DWT_ERROR;

    uwb_tracking_outcome_t outcome =
        uwb_tracking_run_cycle(&default_cfg, &state, 1000);

    zassert_equal(outcome, UWB_TRACKING_MISSED_WINDOW,
        "expected UWB_TRACKING_MISSED_WINDOW; got %d", (int)outcome);
    zassert_equal(mock_starttx_state.called, 1,
        "dwt_starttx() must be called exactly once -- no busy-retry within "
        "the same call; got %d", mock_starttx_state.called);
    zassert_equal(state.blink_count, 0,
        "blink_count must NOT increment for a dropped/missed-window blink");

    /* uwb_tracking.c additionally feeds uwb_cycle_ref_on_miss() on a missed
     * TX window (see uwb_tracking.h's UWB_TRACKING_MISSED_WINDOW doc) so a
     * firmware falling behind schedule re-syncs on a later cycle. */
    zassert_equal(state.cycle_ref.missed, 1,
        "a missed TX window must be recorded against the cycle reference");
}

/* =============================================================================
 * Out-of-range slot config -> UWB_TRACKING_BUILD_ERROR, no TX ever attempted
 * ========================================================================= */
ZTEST(uwb_tracking, test_bad_slot_config_returns_build_error_without_tx)
{
    uwb_tracking_config_t bad_cfg = default_cfg;
    uwb_tracking_state_t state;

    /* slot_idx == slot_count is out of range -- uwb_slot_tx_time() (UWB-251)
     * rejects it with -EINVAL before dw1000_delayed_tx_time() is even
     * called. */
    bad_cfg.slot_idx = bad_cfg.slot_count;

    uwb_tracking_state_init(&state);
    configure_good_sync_rx(0x0001u, 0x01ULL, 0x1000ULL);

    uwb_tracking_outcome_t outcome =
        uwb_tracking_run_cycle(&bad_cfg, &state, 1000);

    zassert_equal(outcome, UWB_TRACKING_BUILD_ERROR,
        "expected UWB_TRACKING_BUILD_ERROR; got %d", (int)outcome);
    zassert_equal(mock_delayedtrxtime_state.called, 0,
        "no TX scheduling attempted for an out-of-range slot_idx");
    zassert_equal(mock_tx_state.writetxdata_called, 0,
        "no blink should be built for an out-of-range slot_idx");
    zassert_equal(state.blink_count, 0, "blink_count must stay at 0");
}

/* =============================================================================
 * NULL cfg / state are rejected without touching the radio
 * ========================================================================= */
ZTEST(uwb_tracking, test_null_args_rejected_without_touching_radio)
{
    uwb_tracking_state_t state;

    uwb_tracking_state_init(&state);

    zassert_equal(uwb_tracking_run_cycle(NULL, &state, 1000),
        UWB_TRACKING_BUILD_ERROR, "NULL cfg must be rejected");
    zassert_equal(uwb_tracking_run_cycle(&default_cfg, NULL, 1000),
        UWB_TRACKING_BUILD_ERROR, "NULL state must be rejected");

    zassert_equal(mock_rxenable_state.called, 0,
        "NULL cfg/state must not touch the radio at all (no dw1000_rx() "
        "attempt); got %d calls", mock_rxenable_state.called);
}

/* =============================================================================
 * uwb_tracking_state_init(): starts "never synced, no blinks yet"
 * ========================================================================= */
ZTEST(uwb_tracking, test_state_init_starts_never_synced)
{
    uwb_tracking_state_t state;
    uint64_t sync_rx_ts;
    uint16_t cycle_seq;

    /* Poison the memory first so the assertions below actually exercise
     * uwb_tracking_state_init(), rather than happening to pass on
     * zero-initialised stack memory. */
    memset(&state, 0xAA, sizeof(state));

    uwb_tracking_state_init(&state);

    zassert_equal(state.blink_count, 0, "blink_count must start at 0");
    zassert_equal(state.mac_seq, 0, "mac_seq must start at 0");
    zassert_false(uwb_cycle_ref_get(&state.cycle_ref, &sync_rx_ts, &cycle_seq),
        "cycle reference must start invalid (never synced)");
}

ZTEST(uwb_tracking, test_state_init_null_is_noop)
{
    /* Must not crash. */
    uwb_tracking_state_init(NULL);
}
