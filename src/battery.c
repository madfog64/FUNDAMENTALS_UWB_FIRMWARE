/*! ----------------------------------------------------------------------------
 * @file    battery.c
 * @brief   Pure conversion arithmetic for the battery voltage monitor
 *          (UWB-322, ADR-0046 point 5).
 *
 * Deliberately free of any Zephyr/hardware include -- this is the unit-test
 * seam (tests/battery) for the raw-ADC-counts -> millivolts/percentage
 * conversion. The Zephyr-facing counterpart that actually reads the SAADC
 * and surfaces the result is battery_zephyr.c.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <stddef.h>

#include "battery.h"

uint16_t battery_raw_to_millivolts(int32_t raw_counts)
{
    /* Clamp to the valid single-ended SAADC range -- see battery.h's
     * documentation of @p raw_counts for why a negative or over-range value
     * can occur (should not happen for a single-ended VDD channel, but a
     * clamp is cheap insurance against a misconfigured channel producing a
     * nonsensical percentage rather than a wrapped/underflowed one). */
    if (raw_counts < 0) {
        raw_counts = 0;
    } else if (raw_counts > BATTERY_ADC_MAX_RAW) {
        raw_counts = BATTERY_ADC_MAX_RAW;
    }

    /* Ideal ADC transfer function: V_in = (raw / 2^BITS) * V_ref_effective,
     * where V_ref_effective = BATTERY_ADC_REFERENCE_MV * BATTERY_ADC_GAIN_DIVISOR
     * (the SAADC's internal 0.6 V reference, scaled up by the reciprocal of
     * the 1/6 gain -- see battery.h header comment for the full derivation).
     * 2^BITS (not 2^BITS - 1) is the correct denominator for this transfer
     * function; using int64_t for the intermediate product avoids any risk
     * of 32-bit overflow (worst case ~14.7M, nowhere close either way, but
     * this keeps the arithmetic obviously safe regardless of future constant
     * changes). */
    int64_t numerator = (int64_t)raw_counts * BATTERY_ADC_REFERENCE_MV * BATTERY_ADC_GAIN_DIVISOR;
    int64_t millivolts = numerator / (1 << BATTERY_ADC_RESOLUTION_BITS);

    return (uint16_t)millivolts;
}

/* Piecewise-linear breakpoints approximating a single-cell Li-based
 * (LiPo/Li-ion) discharge curve -- see battery.h's doc comment for the
 * "not a calibrated fuel-gauge curve" caveat. Ascending by millivolts. */
struct battery_percent_breakpoint {
    uint16_t millivolts;
    uint8_t percent;
};

static const struct battery_percent_breakpoint breakpoints[] = {
    { 3000, 0 },
    { 3400, 20 },
    { 3700, 50 },
    { 3900, 80 },
    { 4200, 100 },
};

#define BATTERY_PERCENT_BREAKPOINT_COUNT \
    (sizeof(breakpoints) / sizeof(breakpoints[0]))

uint8_t battery_millivolts_to_percent(uint16_t millivolts)
{
    /* Below the lowest breakpoint: clamp to 0% rather than extrapolating
     * (a Li-based cell below its cutoff voltage is effectively empty). */
    if (millivolts <= breakpoints[0].millivolts) {
        return breakpoints[0].percent;
    }

    /* Above the highest breakpoint: clamp to 100% (a fresh charge can sit
     * briefly above the "fully charged" breakpoint before settling). */
    if (millivolts >= breakpoints[BATTERY_PERCENT_BREAKPOINT_COUNT - 1].millivolts) {
        return breakpoints[BATTERY_PERCENT_BREAKPOINT_COUNT - 1].percent;
    }

    /* Linear interpolation between the two breakpoints millivolts falls
     * between. */
    for (size_t i = 0; i < BATTERY_PERCENT_BREAKPOINT_COUNT - 1; i++) {
        const struct battery_percent_breakpoint *lo = &breakpoints[i];
        const struct battery_percent_breakpoint *hi = &breakpoints[i + 1];

        if (millivolts >= lo->millivolts && millivolts <= hi->millivolts) {
            int32_t mv_span = hi->millivolts - lo->millivolts;
            int32_t pct_span = hi->percent - lo->percent;
            int32_t mv_offset = millivolts - lo->millivolts;

            return (uint8_t)(lo->percent + (pct_span * mv_offset) / mv_span);
        }
    }

    /* Unreachable given the clamps above, but keeps the compiler happy about
     * a fall-through return path. */
    return breakpoints[BATTERY_PERCENT_BREAKPOINT_COUNT - 1].percent;
}
