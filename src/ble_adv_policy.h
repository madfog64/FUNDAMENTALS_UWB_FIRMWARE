/*! ----------------------------------------------------------------------------
 * @file    ble_adv_policy.h
 * @brief   BLE advertising duty-cycle policy (UWB-320, ADR-0046 point 4).
 *
 * `main.c` used to start connectable BLE advertising unconditionally and
 * never stop it. Per ADR-0046 ("Tag power-management profile & low-power
 * state model"), that is only correct for the bench profile: in the field
 * (low-power) profile, advertising must be *duty-cycled* against the tag's
 * power state --
 *
 *   ACTIVE  (tracking a session)  -- advertising SUPPRESSED. BLE connection
 *           events run on the system work queue and can perturb the tight
 *           TDMA blink timing (ADR-0040 "BLE <-> UWB coexistence note",
 *           ADR-0046 point 4), and advertising costs power for no benefit
 *           while the tag has nothing to say over BLE.
 *   STANDBY (no active session / OFF-pending maintenance window)
 *           -- advertising ENABLED. This is the OTA/maintenance window
 *           ADR-0040 already assumes ("OTA runs in a maintenance/idle
 *           window, not mid-session").
 *
 * The **bench profile** (`!CONFIG_UWB_TAG_LOW_POWER`) is unaffected by any
 * of this: advertising stays always-on, exactly as before this ticket,
 * regardless of the requested state -- see ble_adv_policy_should_advertise().
 *
 * Split across two translation units, mirroring image_health.h/.c's
 * pure-vs-Zephyr split (UWB-266):
 *   ble_adv_policy.c         ble_adv_policy_should_advertise() -- pure
 *                             decision logic, no Zephyr includes,
 *                             host-testable (see tests/ble_adv_policy).
 *   ble_adv_policy_zephyr.c  Everything that actually touches the BLE
 *                             stack (bt_le_adv_start/stop, the connect/
 *                             disconnect callbacks, the re-arm work item)
 *                             -- Zephyr/hardware-only, not unit tested;
 *                             exercised by the real board build + the
 *                             README "Hardware bring-up checklist".
 *
 * Scope note (this is a *mechanism*, not the STANDBY state machine):
 * ble_adv_policy_set_state() is the caller-supplied signal seam the
 * STANDBY power-state machine (UWB-319, a sibling ticket, not yet
 * implemented) will eventually drive from real session/SYNC-loss state.
 * Until that lands, main.c calls it once at boot with a fixed STANDBY
 * default -- see main.c's bt_ready() for why that default is safe.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef BLE_ADV_POLICY_H_
#define BLE_ADV_POLICY_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tag power states this policy reacts to (ADR-0046's three-state
 *        model, narrowed to the two states BLE advertising cares about --
 *        OFF-pending is treated identically to STANDBY, "advertise").
 */
enum ble_adv_policy_state {
    /** Actively tracking a session (listening SYNC + blinking each
     *  superframe). Advertising is suppressed in the low-power profile. */
    BLE_ADV_POLICY_STATE_ACTIVE,

    /** No active session (idle/waiting-to-join), or the OFF-pending
     *  maintenance window. Advertising is enabled -- this is the ADR-0040
     *  OTA/maintenance window. */
    BLE_ADV_POLICY_STATE_STANDBY,
};

/**
 * @brief Pure decision: should BLE advertising be running for this
 *        (build-profile, power-state) combination?
 *
 * Host-testable (ztest/native, no Zephyr kernel/BT stack needed) -- see
 * tests/ble_adv_policy.
 *
 * @param low_power_profile  Whether CONFIG_UWB_TAG_LOW_POWER is enabled for
 *                            this build. When false (bench profile),
 *                            advertising is always wanted regardless of
 *                            @p state -- the pre-UWB-320 behaviour is
 *                            preserved byte-for-byte for that profile.
 * @param state               The requested power state (ignored when
 *                            @p low_power_profile is false).
 * @return true if advertising should be running, false if it should be
 *         suppressed.
 */
bool ble_adv_policy_should_advertise(bool low_power_profile, enum ble_adv_policy_state state);

/**
 * @brief One-time init: wires up the advertising re-arm work item and the
 *        BLE connect/disconnect callbacks.
 *
 * Call once after bt_enable() succeeds (see main.c's bt_ready()), before
 * the first ble_adv_policy_set_state() / ble_adv_enable() / ble_adv_disable()
 * call.
 */
void ble_adv_policy_init(void);

/**
 * @brief Drive the policy from a caller-supplied power state.
 *
 * Bench profile (`!CONFIG_UWB_TAG_LOW_POWER`): always enables advertising,
 * regardless of @p state -- matches the pre-UWB-320 always-on behaviour.
 *
 * Low-power profile: enables advertising for BLE_ADV_POLICY_STATE_STANDBY,
 * disables it for BLE_ADV_POLICY_STATE_ACTIVE.
 *
 * @param state  The tag's current power state.
 */
void ble_adv_policy_set_state(enum ble_adv_policy_state state);

/**
 * @brief Start (or re-arm) connectable BLE advertising.
 *
 * Safe to call when already advertising (stops then restarts, matching the
 * pre-UWB-320 advertise() behaviour). Marks the policy as "advertising
 * allowed" so a subsequent disconnect re-arms advertising instead of
 * leaving the radio silent.
 */
void ble_adv_enable(void);

/**
 * @brief Stop BLE advertising and mark the policy as "advertising not
 *        allowed" so a subsequent disconnect (of an already-established
 *        connection) does not re-arm it.
 *
 * Safe to call when not currently advertising.
 */
void ble_adv_disable(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_ADV_POLICY_H_ */
