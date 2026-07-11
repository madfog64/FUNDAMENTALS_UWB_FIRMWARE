/*! ----------------------------------------------------------------------------
 * @file    battery.h
 * @brief   Battery voltage monitor: nRF52 SAADC VDD -> millivolts/percentage
 *          (UWB-322, ADR-0046 point 5).
 *
 * ADR-0046 point 5 makes battery monitoring a *local* seam for v1: sample the
 * nRF52 SAADC against its internal reference to read VDD, on a low-rate
 * timer (seconds-minutes, not per tracking cycle), and surface the result
 * over the existing MCUmgr `os_mgmt` channel (or a log line) -- NOT over the
 * UWB blink payload (that would need a `contracts/uwb` MINOR bump across
 * three repos, explicitly deferred to "fork B", see the ADR's "Open" section
 * and "Alternatives considered").
 *
 * Split across two translation units, mirroring image_health.h/.c and
 * ble_adv_policy.h/.c's pure-vs-Zephyr split (UWB-266/UWB-320):
 *   battery.c         battery_raw_to_millivolts() / battery_millivolts_to_percent()
 *                       -- pure conversion arithmetic, no Zephyr includes,
 *                       host-testable (see tests/battery).
 *   battery_zephyr.c   Everything that actually touches hardware: the SAADC
 *                       channel read (Zephyr `adc` devicetree API against
 *                       the `NRF_SAADC_VDD` input, see
 *                       boards/nrf52dk_nrf52832.overlay), the low-rate
 *                       k_timer/k_work_delayable sample loop, and the
 *                       os_mgmt "info" custom-hook that surfaces the latest
 *                       reading -- Zephyr/hardware-only, not unit tested;
 *                       exercised by the real board build + the README
 *                       "Hardware bring-up checklist".
 *
 * Conversion assumptions (matched by the devicetree channel config in
 * boards/nrf52dk_nrf52832.overlay -- see battery_zephyr.c for the actual
 * `adc_channel_cfg`):
 *   - SAADC gain     1/6   (BATTERY_ADC_GAIN_DIVISOR = 6)
 *   - SAADC reference internal, 0.6 V (BATTERY_ADC_REFERENCE_MV = 600)
 *   - SAADC resolution 12-bit (BATTERY_ADC_RESOLUTION_BITS = 12)
 *   => full-scale input range = reference / gain = 0.6 V x 6 = 3.6 V, which
 *   comfortably spans the tag's expected VDD range (nominal single-cell
 *   LiPo/coin-cell rail, ~1.8-3.6 V) without needing an external divider.
 *   This is the standard nRF52 "read VDD via SAADC" configuration (see
 *   Nordic's battery/fuel-gauge sample applications for the same gain/
 *   reference/resolution combination).
 *
 * Out of scope (ADR-0046, this ticket):
 *   - Reporting battery via the UWB blink payload (fork B, deferred).
 *   - Low-battery behaviour / shutdown thresholds (a later ticket).
 *   - Percentage curve fidelity: battery_millivolts_to_percent() is a coarse
 *     piecewise-linear approximation for a single-cell Li-based cell, not a
 *     calibrated fuel-gauge curve -- good enough for "roughly how much is
 *     left" over os_mgmt/log, not for a battery-life SLA.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef BATTERY_H_
#define BATTERY_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** SAADC gain divisor: gain = 1/6, so the true input voltage is 6x the
 *  voltage the ADC actually measures against its reference. */
#define BATTERY_ADC_GAIN_DIVISOR 6

/** SAADC internal reference voltage, millivolts (nRF52 fixed 0.6 V internal
 *  reference -- see boards/nrf52dk_nrf52832.overlay's `zephyr,reference =
 *  "ADC_REF_INTERNAL"`). */
#define BATTERY_ADC_REFERENCE_MV 600

/** SAADC sample resolution, bits (see boards/nrf52dk_nrf52832.overlay's
 *  `zephyr,resolution = <12>`). */
#define BATTERY_ADC_RESOLUTION_BITS 12

/** 2^BATTERY_ADC_RESOLUTION_BITS - 1: the maximum raw SAADC count at the
 *  configured resolution (4095 at 12-bit). */
#define BATTERY_ADC_MAX_RAW ((1 << BATTERY_ADC_RESOLUTION_BITS) - 1)

/**
 * @brief Convert a raw SAADC count into millivolts.
 *
 * Pure arithmetic -- no Zephyr includes, host-testable (tests/battery).
 * Assumes the fixed gain/reference/resolution documented above (matched by
 * the devicetree channel config battery_zephyr.c reads through); this is not
 * a generic ADC conversion helper for arbitrary channel configs.
 *
 * @param raw_counts  Raw SAADC sample value, as returned by
 *                     adc_sequence.buffer after adc_read() (0..BATTERY_ADC_MAX_RAW
 *                     for a valid positive-only reading). Values outside that
 *                     range (e.g. a negative differential-mode read that
 *                     leaked through, which should not happen for a
 *                     single-ended VDD channel) are clamped to the nearest
 *                     valid endpoint rather than wrapping/underflowing.
 * @return Millivolts, clamped to [0, BATTERY_ADC_REFERENCE_MV * BATTERY_ADC_GAIN_DIVISOR].
 */
uint16_t battery_raw_to_millivolts(int32_t raw_counts);

/**
 * @brief Coarse battery percentage from a millivolt reading.
 *
 * Piecewise-linear approximation of a single-cell Li-based (LiPo/Li-ion)
 * discharge curve -- NOT a calibrated fuel-gauge curve. Good enough for a
 * rough "how much is left" indicator over os_mgmt/log (this ticket's scope);
 * revisit with a real fuel-gauge IC or measured discharge curve if precision
 * matters later.
 *
 * Breakpoints (millivolts -> percent), linearly interpolated between:
 *   >= 4200 mV -> 100%   (fully charged, upper safety clamp)
 *      3900 mV ->  80%
 *      3700 mV ->  50%
 *      3400 mV ->  20%
 *      3000 mV ->   0%   (nominal single-cell Li empty/cutoff)
 *   <= 3000 mV ->   0%   (lower safety clamp)
 *
 * @param millivolts  Battery voltage in millivolts (e.g. from
 *                     battery_raw_to_millivolts()).
 * @return Percentage in [0, 100].
 */
uint8_t battery_millivolts_to_percent(uint16_t millivolts);

/**
 * @brief One-time init: validates the board's ADC channel is ready,
 *        configures it, and starts the low-rate periodic sample loop.
 *
 * Zephyr/hardware-only -- implemented in battery_zephyr.c, not compiled
 * into the tests/battery host-test binary (see that file's header comment).
 * Call once during application start-up, after the ADC subsystem is
 * available (see main.c).
 *
 * Requires CONFIG_UWB_BATTERY_MONITOR=y and a board devicetree ADC channel
 * (see boards/nrf52dk_nrf52832.overlay's "zephyr,user { io-channels = ...
 * };").
 *
 * @return 0 on success, a negative errno on failure (e.g. -ENODEV if the
 *         ADC controller reports not-ready).
 */
int battery_monitor_init(void);

/**
 * @brief Retrieve the most recent battery reading.
 *
 * Zephyr/hardware-only -- implemented in battery_zephyr.c. Thread-safe
 * (mutex-guarded); safe to call from a different context than the sample
 * timer (e.g. the os_mgmt custom info hook, see battery_zephyr.c).
 *
 * @param[out] millivolts  Set to the latest millivolt reading if the
 *                          function returns true (pass NULL to ignore).
 * @param[out] percent     Set to the latest coarse percentage if the
 *                          function returns true (pass NULL to ignore).
 * @return true if at least one sample cycle has completed since boot,
 *         false if no reading is available yet.
 */
bool battery_monitor_get_latest(uint16_t *millivolts, uint8_t *percent);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_H_ */
