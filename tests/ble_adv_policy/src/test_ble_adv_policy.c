/*! ----------------------------------------------------------------------------
 * @file    test_ble_adv_policy.c
 * @brief   ztest suite: BLE advertising duty-cycle policy decision logic
 *          (UWB-320, ADR-0046 point 4)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real BLE
 * stack).
 *
 * Exercises only ble_adv_policy_should_advertise() -- the pure decision
 * function. ble_adv_policy_zephyr.c (the real bt_le_adv_start/stop wiring)
 * is not compiled into this test binary; it is exercised by the real board
 * build + the on-device bring-up checklist (README.md).
 *
 * Covers, per the UWB-320 acceptance criteria:
 *   - Bench profile (low_power_profile=false): advertising wanted in both
 *     ACTIVE and STANDBY -- "bench build advertising unchanged (always on)".
 *   - Low-power profile (low_power_profile=true): advertising suppressed in
 *     ACTIVE, wanted in STANDBY -- "low-power build does not advertise
 *     while ACTIVE and advertises in STANDBY".
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <zephyr/ztest.h>

#include "ble_adv_policy.h"

ZTEST_SUITE(ble_adv_policy, NULL, NULL, NULL, NULL, NULL);

ZTEST(ble_adv_policy, test_bench_profile_advertises_while_active)
{
	zassert_true(
		ble_adv_policy_should_advertise(false, BLE_ADV_POLICY_STATE_ACTIVE),
		"bench profile must keep advertising even while ACTIVE");
}

ZTEST(ble_adv_policy, test_bench_profile_advertises_while_standby)
{
	zassert_true(
		ble_adv_policy_should_advertise(false, BLE_ADV_POLICY_STATE_STANDBY),
		"bench profile must advertise while STANDBY");
}

ZTEST(ble_adv_policy, test_low_power_profile_suppresses_while_active)
{
	zassert_false(
		ble_adv_policy_should_advertise(true, BLE_ADV_POLICY_STATE_ACTIVE),
		"low-power profile must suppress advertising while ACTIVE");
}

ZTEST(ble_adv_policy, test_low_power_profile_advertises_while_standby)
{
	zassert_true(
		ble_adv_policy_should_advertise(true, BLE_ADV_POLICY_STATE_STANDBY),
		"low-power profile must advertise while STANDBY");
}
