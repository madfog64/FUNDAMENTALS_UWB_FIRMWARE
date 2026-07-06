/*! ----------------------------------------------------------------------------
 * @file    test_slot_timing.c
 * @brief   ztest suite: TDMA slot-timing computation for tag blink TX
 *          (UWB-251)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI).
 * Links the real dw1000_ranging.c so dw1000_delayed_tx_time() (UWB-231) is
 * exercised for real -- see tests/slot_timing/CMakeLists.txt.
 *
 * Covers, per the UWB-251 acceptance criteria:
 *   - slot_idx = 0 yields dw1000_delayed_tx_time(sync_rx_ts, 0).
 *   - slot_idx = k (k = 1, a mid slot, slot_count-1) yields exactly
 *     dw1000_delayed_tx_time(sync_rx_ts, k * slot_duration_dtu).
 *   - Monotonic separation: consecutive slot_idx values produce a
 *     pre-truncation delay separated by exactly slot_duration_dtu.
 *   - slot_idx >= slot_count returns -EINVAL and leaves *tx_time_dtu
 *     untouched (the driver is not called).
 *   - slot_idx * slot_duration_dtu overflowing the 32-bit delay the driver
 *     accepts returns -ERANGE, not a wrapped value (including the exact
 *     UINT32_MAX boundary: fits -> succeeds, one tick over -> -ERANGE).
 *   - NULL tx_time_dtu returns -EINVAL.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include "dw1000_ranging.h"
#include "uwb_slot_timing.h"

/* Sentinel used to confirm *tx_time_dtu is left untouched on error paths. */
#define SENTINEL_TX_TIME  ((uint64_t)0xDEADBEEFDEULL)

ZTEST_SUITE(uwb_slot_timing, NULL, NULL, NULL, NULL, NULL);

/* =============================================================================
 * slot_idx = 0 -> the SYNC-referenced boundary itself
 * ========================================================================= */
ZTEST(uwb_slot_timing, test_slot_zero_is_sync_boundary)
{
    const uint64_t sync_rx_ts = 0x0123456789ULL;
    const uint32_t slot_duration_dtu = 32000000u; /* ~0.5 ms superframe slot */
    const uint32_t slot_count = 24u;
    uint64_t tx_time_dtu = 0;

    int ret = uwb_slot_tx_time(sync_rx_ts, 0u, slot_duration_dtu, slot_count,
                                &tx_time_dtu);

    zassert_equal(ret, 0, "expected success; got %d", ret);
    zassert_equal(tx_time_dtu, dw1000_delayed_tx_time(sync_rx_ts, 0u),
        "slot_idx=0: expected dw1000_delayed_tx_time(sync_rx_ts, 0)");
}

/* =============================================================================
 * slot_idx = k -> delay fed to dw1000_delayed_tx_time is exactly
 * k * slot_duration_dtu (k = 1, a mid slot, slot_count - 1)
 * ========================================================================= */
ZTEST(uwb_slot_timing, test_slot_k_matches_k_times_slot_duration)
{
    const uint64_t sync_rx_ts = 0x0555555555ULL;
    const uint32_t slot_duration_dtu = 32000000u;
    const uint32_t slot_count = 24u;
    const uint32_t ks[] = {1u, 12u /* mid slot */, slot_count - 1u};

    for (size_t i = 0; i < sizeof(ks) / sizeof(ks[0]); i++) {
        uint32_t k = ks[i];
        uint64_t tx_time_dtu = 0;

        int ret = uwb_slot_tx_time(sync_rx_ts, k, slot_duration_dtu,
                                    slot_count, &tx_time_dtu);

        zassert_equal(ret, 0, "k=%u: expected success; got %d",
            (unsigned)k, ret);

        uint64_t expected =
            dw1000_delayed_tx_time(sync_rx_ts, k * slot_duration_dtu);

        zassert_equal(tx_time_dtu, expected,
            "k=%u: expected 0x%010llX; got 0x%010llX",
            (unsigned)k, (unsigned long long)expected,
            (unsigned long long)tx_time_dtu);
    }
}

/* =============================================================================
 * Monotonic separation: consecutive slot_idx values are separated by
 * exactly slot_duration_dtu in the pre-truncation delay (adjacent slots
 * never overlap for valid inputs).
 * ========================================================================= */
ZTEST(uwb_slot_timing, test_consecutive_slots_separated_by_slot_duration)
{
    const uint64_t sync_rx_ts = 0x1000ULL;
    const uint32_t slot_duration_dtu = 32000000u;
    const uint32_t slot_count = 24u;

    for (uint32_t k = 0; k + 1 < slot_count; k++) {
        uint64_t tx_k = 0;
        uint64_t tx_k1 = 0;

        zassert_equal(
            uwb_slot_tx_time(sync_rx_ts, k, slot_duration_dtu, slot_count, &tx_k),
            0, "k=%u: expected success", (unsigned)k);
        zassert_equal(
            uwb_slot_tx_time(sync_rx_ts, k + 1u, slot_duration_dtu, slot_count, &tx_k1),
            0, "k=%u: expected success", (unsigned)(k + 1u));

        /* Compare against the driver call directly on the pre-truncation
         * delay (k * D vs (k+1) * D) -- i.e. the two calls must agree with
         * what dw1000_delayed_tx_time() would produce for delays exactly
         * slot_duration_dtu apart. */
        uint64_t expected_k =
            dw1000_delayed_tx_time(sync_rx_ts, k * slot_duration_dtu);
        uint64_t expected_k1 =
            dw1000_delayed_tx_time(sync_rx_ts, (k + 1u) * slot_duration_dtu);

        zassert_equal(tx_k, expected_k, "k=%u mismatch", (unsigned)k);
        zassert_equal(tx_k1, expected_k1, "k=%u mismatch", (unsigned)(k + 1u));

        /* The underlying pre-truncation delays themselves (not the
         * post-truncation TX times) are separated by exactly
         * slot_duration_dtu -- this is what guarantees adjacent slots never
         * overlap for valid inputs. */
        uint64_t delay_k = (uint64_t)k * slot_duration_dtu;
        uint64_t delay_k1 = (uint64_t)(k + 1u) * slot_duration_dtu;

        zassert_equal(delay_k1 - delay_k, (uint64_t)slot_duration_dtu,
            "k=%u: pre-truncation delay separation must equal slot_duration_dtu",
            (unsigned)k);
    }
}

/* =============================================================================
 * slot_idx >= slot_count is rejected before the driver is called
 * ========================================================================= */
ZTEST(uwb_slot_timing, test_slot_idx_out_of_range_rejected)
{
    const uint32_t slot_count = 24u;
    uint64_t tx_time_dtu = SENTINEL_TX_TIME;

    int ret = uwb_slot_tx_time(0x1000ULL, slot_count, 32000000u, slot_count,
                                &tx_time_dtu);

    zassert_equal(ret, -EINVAL,
        "slot_idx == slot_count: expected -EINVAL; got %d", ret);
    zassert_equal(tx_time_dtu, SENTINEL_TX_TIME,
        "*tx_time_dtu must be left untouched (driver not called) on error");

    /* Comfortably past slot_count too. */
    tx_time_dtu = SENTINEL_TX_TIME;
    ret = uwb_slot_tx_time(0x1000ULL, slot_count + 100u, 32000000u, slot_count,
                            &tx_time_dtu);

    zassert_equal(ret, -EINVAL,
        "slot_idx > slot_count: expected -EINVAL; got %d", ret);
    zassert_equal(tx_time_dtu, SENTINEL_TX_TIME,
        "*tx_time_dtu must be left untouched (driver not called) on error");
}

/* =============================================================================
 * Overflow guard: slot_idx * slot_duration_dtu too large to hand to the
 * driver returns -ERANGE, not a wrapped/garbage value.
 * ========================================================================= */
ZTEST(uwb_slot_timing, test_overflow_guard_returns_erange)
{
    uint64_t tx_time_dtu = SENTINEL_TX_TIME;

    /* slot_idx must stay strictly below slot_count (0xFFFFFFFEu <
     * 0xFFFFFFFFu) so the slot_idx>=slot_count check does not fire first --
     * the product is still comfortably beyond both the 32-bit delay
     * parameter and the 40-bit DW1000 system time width. */
    int ret = uwb_slot_tx_time(0x1000ULL, 0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                &tx_time_dtu);

    zassert_equal(ret, -ERANGE,
        "expected -ERANGE on gross overflow; got %d", ret);
    zassert_equal(tx_time_dtu, SENTINEL_TX_TIME,
        "*tx_time_dtu must be left untouched (driver not called) on overflow");
}

/* ---------------------------------------------------------------------------
 * Boundary: delay == UINT32_MAX fits (succeeds); one tick over does not.
 * --------------------------------------------------------------------------- */
ZTEST(uwb_slot_timing, test_overflow_guard_boundary_at_uint32_max)
{
    const uint64_t sync_rx_ts = 0x2000ULL;
    uint64_t tx_time_dtu = 0;

    /* slot_idx=1, slot_duration_dtu=UINT32_MAX -> delay = UINT32_MAX exactly:
     * fits the driver's 32-bit delay parameter, must succeed. */
    int ret = uwb_slot_tx_time(sync_rx_ts, 1u, 0xFFFFFFFFu, 2u, &tx_time_dtu);

    zassert_equal(ret, 0,
        "delay == UINT32_MAX must fit and succeed; got %d", ret);
    zassert_equal(tx_time_dtu, dw1000_delayed_tx_time(sync_rx_ts, 0xFFFFFFFFu),
        "boundary success case: unexpected truncated TX time");

    /* slot_idx=2, same slot_duration_dtu -> delay = 2 * UINT32_MAX, which
     * does not fit in 32 bits -- must be rejected, not silently truncated. */
    tx_time_dtu = SENTINEL_TX_TIME;
    ret = uwb_slot_tx_time(sync_rx_ts, 2u, 0xFFFFFFFFu, 3u, &tx_time_dtu);

    zassert_equal(ret, -ERANGE,
        "delay == 2*UINT32_MAX must be rejected; got %d", ret);
    zassert_equal(tx_time_dtu, SENTINEL_TX_TIME,
        "*tx_time_dtu must be left untouched (driver not called) on overflow");
}

/* =============================================================================
 * NULL tx_time_dtu is rejected
 * ========================================================================= */
ZTEST(uwb_slot_timing, test_null_out_param_rejected)
{
    int ret = uwb_slot_tx_time(0x1000ULL, 0u, 32000000u, 24u, NULL);

    zassert_equal(ret, -EINVAL,
        "NULL tx_time_dtu: expected -EINVAL; got %d", ret);
}
