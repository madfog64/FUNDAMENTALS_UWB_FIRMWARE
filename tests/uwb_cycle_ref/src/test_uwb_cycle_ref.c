/*! ----------------------------------------------------------------------------
 * @file    test_uwb_cycle_ref.c
 * @brief   ztest suite: tag-side maintained cycle reference (UWB-243)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI).
 *
 * Covers, per the UWB-243 acceptance criteria:
 *   - init() -> get() returns false (never synced).
 *   - on_sync() -> get() returns true with the fed sync_rx_ts/cycle_seq.
 *   - Contiguous on_sync() calls keep missed == 0 and update to the latest
 *     edge.
 *   - on_miss() x CONFIG_UWB_SYNC_MAX_MISSED stays valid; one more call
 *     invalidates the reference (get() -> false).
 *   - After invalidation, a fresh on_sync() restores validity and resets
 *     missed.
 *   - A cycle_seq discontinuity is still accepted (valid, updates) --
 *     exercises the LOG_WRN() warning path (stubbed by mock_log.c).
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <zephyr/ztest.h>

#include "dw1000_sync.h"
#include "uwb_cycle_ref.h"

/* ---------------------------------------------------------------------------
 * Suite setup / teardown
 * --------------------------------------------------------------------------- */
static uwb_cycle_ref_t ref;

static void before_each(void *fixture)
{
    (void)fixture;
    uwb_cycle_ref_init(&ref);
}

ZTEST_SUITE(uwb_cycle_ref, NULL, NULL, before_each, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Helper: build a uwb_sync_info_t
 * --------------------------------------------------------------------------- */
static uwb_sync_info_t make_sync_info(uint64_t rx_ts, uint16_t cycle_seq,
                                       uint64_t master_tx_ts)
{
    uwb_sync_info_t s = {
        .rx_ts = rx_ts,
        .cycle_seq = cycle_seq,
        .master_tx_ts = master_tx_ts,
    };
    return s;
}

/* =============================================================================
 * init() / get()
 * ========================================================================= */

ZTEST(uwb_cycle_ref, test_init_then_get_returns_false)
{
    uint64_t sync_rx_ts = 0xDEADBEEFULL;
    uint16_t cycle_seq = 0xDEAD;

    bool ok = uwb_cycle_ref_get(&ref, &sync_rx_ts, &cycle_seq);

    zassert_false(ok, "get() must return false before any on_sync()");
}

ZTEST(uwb_cycle_ref, test_get_rejects_null_ref)
{
    uint64_t sync_rx_ts;
    uint16_t cycle_seq;

    bool ok = uwb_cycle_ref_get(NULL, &sync_rx_ts, &cycle_seq);

    zassert_false(ok, "get() must return false for a NULL reference");
}

/* =============================================================================
 * on_sync() -- basic acquisition
 * ========================================================================= */

ZTEST(uwb_cycle_ref, test_on_sync_then_get_returns_true_with_fed_values)
{
    const uint64_t golden_rx_ts = 0x0102030405ULL;
    const uint16_t golden_cycle_seq = 0x0007u;
    uwb_sync_info_t s = make_sync_info(golden_rx_ts, golden_cycle_seq, 0x99u);

    uwb_cycle_ref_on_sync(&ref, &s);

    uint64_t sync_rx_ts = 0;
    uint16_t cycle_seq = 0;
    bool ok = uwb_cycle_ref_get(&ref, &sync_rx_ts, &cycle_seq);

    zassert_true(ok, "get() must return true after on_sync()");
    zassert_equal(sync_rx_ts, golden_rx_ts,
        "sync_rx_ts: expected 0x%010llX; got 0x%010llX",
        (unsigned long long)golden_rx_ts, (unsigned long long)sync_rx_ts);
    zassert_equal(cycle_seq, golden_cycle_seq,
        "cycle_seq: expected 0x%04X; got 0x%04X",
        golden_cycle_seq, cycle_seq);
}

ZTEST(uwb_cycle_ref, test_on_sync_ignores_null_args)
{
    uwb_sync_info_t s = make_sync_info(0x01ULL, 0x0001u, 0x00u);

    /* Neither call should crash; state must remain "never synced". */
    uwb_cycle_ref_on_sync(NULL, &s);
    uwb_cycle_ref_on_sync(&ref, NULL);

    zassert_false(uwb_cycle_ref_get(&ref, NULL, NULL),
        "on_sync() with a NULL argument must not acquire the reference");
}

/* =============================================================================
 * Contiguous on_sync() -- missed stays 0, reference updates to latest edge
 * ========================================================================= */

ZTEST(uwb_cycle_ref, test_contiguous_on_sync_keeps_missed_zero_and_updates)
{
    uwb_sync_info_t s1 = make_sync_info(0x1000ULL, 0x0001u, 0x00u);
    uwb_sync_info_t s2 = make_sync_info(0x2000ULL, 0x0002u, 0x00u);
    uwb_sync_info_t s3 = make_sync_info(0x3000ULL, 0x0003u, 0x00u);

    uwb_cycle_ref_on_sync(&ref, &s1);
    zassert_equal(ref.missed, 0, "missed must be 0 after on_sync()");

    uwb_cycle_ref_on_sync(&ref, &s2);
    zassert_equal(ref.missed, 0, "missed must stay 0 across contiguous syncs");

    uwb_cycle_ref_on_sync(&ref, &s3);
    zassert_equal(ref.missed, 0, "missed must stay 0 across contiguous syncs");

    uint64_t sync_rx_ts = 0;
    uint16_t cycle_seq = 0;
    bool ok = uwb_cycle_ref_get(&ref, &sync_rx_ts, &cycle_seq);

    zassert_true(ok, "get() must return true");
    zassert_equal(sync_rx_ts, s3.rx_ts,
        "reference must track the latest sync edge (rx_ts)");
    zassert_equal(cycle_seq, s3.cycle_seq,
        "reference must track the latest sync edge (cycle_seq)");
}

/* =============================================================================
 * on_miss() -- tolerance and invalidation at CONFIG_UWB_SYNC_MAX_MISSED
 * ========================================================================= */

ZTEST(uwb_cycle_ref, test_on_miss_up_to_max_stays_valid)
{
    uwb_sync_info_t s = make_sync_info(0xABCDULL, 0x0001u, 0x00u);
    int i;

    uwb_cycle_ref_on_sync(&ref, &s);

    for (i = 0; i < CONFIG_UWB_SYNC_MAX_MISSED; i++) {
        uwb_cycle_ref_on_miss(&ref);
        zassert_true(uwb_cycle_ref_get(&ref, NULL, NULL),
            "reference must stay valid at missed count %d "
            "(CONFIG_UWB_SYNC_MAX_MISSED=%d)",
            i + 1, CONFIG_UWB_SYNC_MAX_MISSED);
    }
}

ZTEST(uwb_cycle_ref, test_on_miss_one_more_than_max_invalidates)
{
    uwb_sync_info_t s = make_sync_info(0xABCDULL, 0x0001u, 0x00u);
    int i;

    uwb_cycle_ref_on_sync(&ref, &s);

    for (i = 0; i < CONFIG_UWB_SYNC_MAX_MISSED; i++) {
        uwb_cycle_ref_on_miss(&ref);
    }
    zassert_true(uwb_cycle_ref_get(&ref, NULL, NULL),
        "reference must still be valid at exactly CONFIG_UWB_SYNC_MAX_MISSED "
        "missed syncs");

    /* One more miss than the configured tolerance. */
    uwb_cycle_ref_on_miss(&ref);

    zassert_false(uwb_cycle_ref_get(&ref, NULL, NULL),
        "reference must be invalid after exceeding "
        "CONFIG_UWB_SYNC_MAX_MISSED consecutive missed syncs");
}

ZTEST(uwb_cycle_ref, test_on_miss_ignores_null_ref)
{
    /* Must not crash. */
    uwb_cycle_ref_on_miss(NULL);
}

/* =============================================================================
 * Re-acquisition after invalidation
 * ========================================================================= */

ZTEST(uwb_cycle_ref, test_fresh_on_sync_after_invalidation_restores_validity)
{
    uwb_sync_info_t s1 = make_sync_info(0x1111ULL, 0x0001u, 0x00u);
    int i;

    uwb_cycle_ref_on_sync(&ref, &s1);

    /* Drive missed past the tolerance to invalidate. */
    for (i = 0; i <= CONFIG_UWB_SYNC_MAX_MISSED; i++) {
        uwb_cycle_ref_on_miss(&ref);
    }
    zassert_false(uwb_cycle_ref_get(&ref, NULL, NULL),
        "precondition: reference must be invalid before re-sync");

    const uint64_t golden_rx_ts = 0x9999ULL;
    const uint16_t golden_cycle_seq = 0x0100u;
    uwb_sync_info_t s2 = make_sync_info(golden_rx_ts, golden_cycle_seq, 0x00u);

    uwb_cycle_ref_on_sync(&ref, &s2);

    uint64_t sync_rx_ts = 0;
    uint16_t cycle_seq = 0;
    bool ok = uwb_cycle_ref_get(&ref, &sync_rx_ts, &cycle_seq);

    zassert_true(ok, "a fresh on_sync() after invalidation must restore validity");
    zassert_equal(sync_rx_ts, golden_rx_ts, "restored reference must carry the new rx_ts");
    zassert_equal(cycle_seq, golden_cycle_seq,
        "restored reference must carry the new cycle_seq");
    zassert_equal(ref.missed, 0,
        "on_sync() must reset missed to 0 even after invalidation");
}

/* =============================================================================
 * cycle_seq discontinuity -- still accepted (warning path)
 * ========================================================================= */

ZTEST(uwb_cycle_ref, test_cycle_seq_discontinuity_still_accepted)
{
    uwb_sync_info_t s1 = make_sync_info(0x1000ULL, 0x0005u, 0x00u);
    /* Not 0x0006 (expected next) -- deliberate jump. */
    const uint16_t jumped_cycle_seq = 0x000Au;
    const uint64_t jumped_rx_ts = 0x2000ULL;
    uwb_sync_info_t s2 = make_sync_info(jumped_rx_ts, jumped_cycle_seq, 0x00u);

    uwb_cycle_ref_on_sync(&ref, &s1);
    zassert_equal(ref.missed, 0, "missed must be 0 after the first on_sync()");

    /* Must not crash / assert internally; must still re-anchor to s2 despite
     * the discontinuity (exercises the LOG_WRN() path via mock_log.c). */
    uwb_cycle_ref_on_sync(&ref, &s2);

    uint64_t sync_rx_ts = 0;
    uint16_t cycle_seq = 0;
    bool ok = uwb_cycle_ref_get(&ref, &sync_rx_ts, &cycle_seq);

    zassert_true(ok, "reference must remain valid across a cycle_seq jump");
    zassert_equal(sync_rx_ts, jumped_rx_ts,
        "reference must re-anchor to the jumped sync's rx_ts");
    zassert_equal(cycle_seq, jumped_cycle_seq,
        "reference must re-anchor to the jumped sync's cycle_seq");
    zassert_equal(ref.missed, 0, "missed must reset to 0 despite the jump");
}

ZTEST(uwb_cycle_ref, test_cycle_seq_wraparound_is_not_a_discontinuity)
{
    /* 0xFFFF -> 0x0000 is the expected (non-jump) wraparound case. */
    uwb_sync_info_t s1 = make_sync_info(0x1000ULL, 0xFFFFu, 0x00u);
    uwb_sync_info_t s2 = make_sync_info(0x2000ULL, 0x0000u, 0x00u);

    uwb_cycle_ref_on_sync(&ref, &s1);
    uwb_cycle_ref_on_sync(&ref, &s2);

    uint16_t cycle_seq = 0;
    bool ok = uwb_cycle_ref_get(&ref, NULL, &cycle_seq);

    zassert_true(ok, "reference must remain valid across the 16-bit wrap");
    zassert_equal(cycle_seq, 0x0000u,
        "reference must track the wrapped cycle_seq");
    zassert_equal(ref.missed, 0,
        "missed must be 0 -- wraparound is the expected next value, not a "
        "jump");
}
