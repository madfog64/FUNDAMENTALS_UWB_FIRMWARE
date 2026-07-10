/*! ----------------------------------------------------------------------------
 * @file    ble_adv_policy.c
 * @brief   Pure decision logic for the BLE advertising duty-cycle policy
 *          (UWB-320, ADR-0046 point 4).
 *
 * Deliberately free of any Zephyr/BT-stack include -- this is the unit-test
 * seam (tests/ble_adv_policy) for the policy's on/off decision. The
 * Zephyr-facing counterpart that actually starts/stops advertising is
 * ble_adv_policy_zephyr.c.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include "ble_adv_policy.h"

bool ble_adv_policy_should_advertise(bool low_power_profile, enum ble_adv_policy_state state)
{
    if (!low_power_profile) {
        /* Bench profile: always-on advertising, unchanged from before
         * UWB-320 -- the requested state is not consulted. */
        return true;
    }

    /* Low-power profile (ADR-0046 point 4): suppress only while ACTIVE. */
    return state != BLE_ADV_POLICY_STATE_ACTIVE;
}
