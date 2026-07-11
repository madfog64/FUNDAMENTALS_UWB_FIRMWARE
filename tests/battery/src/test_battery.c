/*! ----------------------------------------------------------------------------
 * @file    test_battery.c
 * @brief   ztest suite: battery raw-ADC-counts -> millivolts/percentage
 *          conversion (UWB-322, ADR-0046 point 5)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SAADC).
 *
 * Exercises only battery_raw_to_millivolts() and
 * battery_millivolts_to_percent() -- the pure conversion functions.
 * battery_zephyr.c (the real SAADC read + timer + os_mgmt wiring) is not
 * compiled into this test binary; it is exercised by the real board build +
 * the on-device bring-up checklist (README.md's "confirm reported mV tracks
 * a bench supply sweep" acceptance criterion).
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <zephyr/ztest.h>

#include "battery.h"

ZTEST_SUITE(battery, NULL, NULL, NULL, NULL, NULL);

/* --- battery_raw_to_millivolts() ----------------------------------------- */

ZTEST(battery, test_raw_zero_is_zero_millivolts)
{
	zassert_equal(0, battery_raw_to_millivolts(0),
		      "raw=0 must convert to 0 mV");
}

ZTEST(battery, test_raw_half_scale_is_half_of_full_scale_voltage)
{
	/* raw = 2048 (2^12 / 2) -> mv = 2048 * 3600 / 4096 = 1800, an exact
	 * (no-remainder) integer division -- a clean check that the gain/
	 * reference scaling in battery.h is applied correctly, independent
	 * of rounding behaviour at the top of the range (covered by the
	 * near-full-scale test below). */
	zassert_equal(1800, battery_raw_to_millivolts(2048),
		      "raw=2048 (half scale) must convert to 1800 mV");
}

ZTEST(battery, test_raw_max_valid_is_near_full_scale)
{
	/* raw = BATTERY_ADC_MAX_RAW (4095, i.e. 2^12 - 1) -> mv =
	 * 4095 * 3600 / 4096 = 3599 (integer division truncates 3599.12).
	 * The true full-scale voltage (3600 mV) is only reached in the limit
	 * as raw approaches 2^12 -- 4095 is the highest representable raw
	 * count, one shy of that limit. */
	zassert_equal(3599, battery_raw_to_millivolts(BATTERY_ADC_MAX_RAW),
		      "raw=BATTERY_ADC_MAX_RAW must convert to 3599 mV");
}

ZTEST(battery, test_raw_negative_clamps_to_zero)
{
	zassert_equal(0, battery_raw_to_millivolts(-100),
		      "a negative raw count must clamp to 0 mV, not underflow");
}

ZTEST(battery, test_raw_over_max_clamps_to_max_raw_conversion)
{
	/* An over-range raw count (e.g. a misconfigured differential-mode
	 * leak) must clamp to the same result as the maximum valid raw
	 * count, not produce a nonsensical (or out-of-range) millivolt
	 * value. */
	zassert_equal(battery_raw_to_millivolts(BATTERY_ADC_MAX_RAW),
		      battery_raw_to_millivolts(BATTERY_ADC_MAX_RAW + 5000),
		      "an over-range raw count must clamp to BATTERY_ADC_MAX_RAW's conversion");
}

ZTEST(battery, test_raw_to_millivolts_is_monotonic)
{
	uint16_t previous_mv = battery_raw_to_millivolts(0);

	for (int32_t raw = 256; raw <= BATTERY_ADC_MAX_RAW; raw += 256) {
		uint16_t mv = battery_raw_to_millivolts(raw);

		zassert_true(mv >= previous_mv,
			     "millivolts must not decrease as raw counts increase "
			     "(raw=%d gave %u mV, previous was %u mV)",
			     raw, mv, previous_mv);
		previous_mv = mv;
	}
}

/* --- battery_millivolts_to_percent() -------------------------------------- */

ZTEST(battery, test_percent_clamps_below_lowest_breakpoint)
{
	zassert_equal(0, battery_millivolts_to_percent(2500),
		      "below the lowest breakpoint (3000 mV) must clamp to 0%%");
}

ZTEST(battery, test_percent_at_lowest_breakpoint)
{
	zassert_equal(0, battery_millivolts_to_percent(3000),
		      "3000 mV (the empty/cutoff breakpoint) must report 0%%");
}

ZTEST(battery, test_percent_at_named_breakpoints)
{
	zassert_equal(20, battery_millivolts_to_percent(3400),
		      "3400 mV breakpoint must report 20%%");
	zassert_equal(50, battery_millivolts_to_percent(3700),
		      "3700 mV breakpoint must report 50%%");
	zassert_equal(80, battery_millivolts_to_percent(3900),
		      "3900 mV breakpoint must report 80%%");
	zassert_equal(100, battery_millivolts_to_percent(4200),
		      "4200 mV breakpoint must report 100%%");
}

ZTEST(battery, test_percent_clamps_above_highest_breakpoint)
{
	zassert_equal(100, battery_millivolts_to_percent(4500),
		      "above the highest breakpoint (4200 mV) must clamp to 100%%");
}

ZTEST(battery, test_percent_interpolates_between_breakpoints)
{
	/* 3200 mV is midway between the 3000 mV (0%) and 3400 mV (20%)
	 * breakpoints -> expect 10%%. */
	zassert_equal(10, battery_millivolts_to_percent(3200),
		      "3200 mV must interpolate to 10%% between the 3000/3400 breakpoints");

	/* 3550 mV is midway between the 3400 mV (20%) and 3700 mV (50%)
	 * breakpoints -> expect 35%%. */
	zassert_equal(35, battery_millivolts_to_percent(3550),
		      "3550 mV must interpolate to 35%% between the 3400/3700 breakpoints");

	/* 3800 mV is midway between the 3700 mV (50%) and 3900 mV (80%)
	 * breakpoints -> expect 65%%. */
	zassert_equal(65, battery_millivolts_to_percent(3800),
		      "3800 mV must interpolate to 65%% between the 3700/3900 breakpoints");

	/* 4050 mV is midway between the 3900 mV (80%) and 4200 mV (100%)
	 * breakpoints -> expect 90%%. */
	zassert_equal(90, battery_millivolts_to_percent(4050),
		      "4050 mV must interpolate to 90%% between the 3900/4200 breakpoints");
}

ZTEST(battery, test_percent_is_monotonic)
{
	uint8_t previous_pct = battery_millivolts_to_percent(2000);

	for (uint16_t mv = 2000; mv <= 5000; mv += 50) {
		uint8_t pct = battery_millivolts_to_percent(mv);

		zassert_true(pct >= previous_pct,
			     "percent must not decrease as millivolts increase "
			     "(mv=%u gave %u%%, previous was %u%%)",
			     mv, pct, previous_pct);
		previous_pct = pct;
	}
}
