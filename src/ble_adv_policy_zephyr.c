/*! ----------------------------------------------------------------------------
 * @file    ble_adv_policy_zephyr.c
 * @brief   Zephyr-side BLE advertising duty-cycle wiring (UWB-320,
 *          ADR-0046 point 4).
 *
 * Wraps the advertise / bt_le_adv_start()/bt_le_adv_stop() logic that used
 * to live directly in main.c (UWB-265), and drives it from
 * ble_adv_policy_should_advertise() (ble_adv_policy.c) instead of always
 * starting advertising unconditionally.
 *
 * Not compiled into the tests/ble_adv_policy host-test binary -- this file
 * needs the real Zephyr BT stack (bt_le_adv_start/stop, bt_conn callbacks),
 * which is not available on the 'unit_testing' platform. Exercised by the
 * real board build + the README "Hardware bring-up checklist".
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

#include "ble_adv_policy.h"

LOG_MODULE_REGISTER(ble_adv_policy, LOG_LEVEL_DBG);

/* Advertising is (re)started from a work item so it can be re-armed from the
 * disconnect callback (which runs in the BT RX thread, not a context that
 * should block starting a new advertising set directly) -- carried forward
 * unchanged from the pre-UWB-320 main.c.
 */
static struct k_work advertise_work;

/* Tracks whether the policy currently wants advertising running. Guards the
 * disconnect callback's re-arm: without this, a disconnect that happens
 * while ACTIVE (low-power profile) would blindly resume advertising,
 * defeating the whole point of suppressing it.
 */
static bool adv_allowed;

/* Minimal advertising data: flags + the device name (CONFIG_BT_DEVICE_NAME,
 * "UWB-Tag"). The SMP GATT service itself (advertised implicitly by being
 * connectable + discoverable via GATT service discovery) is registered by
 * CONFIG_MCUMGR_TRANSPORT_BT; it does not need to be listed in the
 * advertising payload for a client to find it after connecting.
 */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void advertise_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    int err;

    bt_le_adv_stop();

    err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        LOG_ERR("BLE advertising failed to start (err %d)", err);
        return;
    }

    LOG_INF("BLE advertising started as \"%s\"", CONFIG_BT_DEVICE_NAME);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    ARG_UNUSED(conn);

    if (err) {
        LOG_ERR("BLE connection failed (err 0x%02x)", err);
        return;
    }

    LOG_INF("BLE central connected — SMP/MCUmgr service available");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);

    LOG_INF("BLE central disconnected (reason 0x%02x)", reason);

    if (!adv_allowed) {
        /* Suppressed (e.g. the tag entered ACTIVE tracking while a BLE
         * central was still connected) -- do not re-arm. Concurrent
         * OTA-during-tracking is out of scope (ADR-0040 defers it); this
         * just avoids silently undoing that suppression on disconnect. */
        LOG_INF("Not resuming advertising — suppressed by power-state policy (ADR-0046)");
        return;
    }

    LOG_INF("Resuming advertising");
    k_work_submit(&advertise_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

void ble_adv_policy_init(void)
{
    k_work_init(&advertise_work, advertise_work_handler);
}

void ble_adv_enable(void)
{
    adv_allowed = true;
    k_work_submit(&advertise_work);
}

void ble_adv_disable(void)
{
    adv_allowed = false;

    int err = bt_le_adv_stop();

    if (err && err != -EALREADY) {
        LOG_WRN("bt_le_adv_stop() returned unexpected err %d while disabling", err);
    }

    LOG_INF("BLE advertising suppressed (ADR-0046 point 4)");
}

void ble_adv_policy_set_state(enum ble_adv_policy_state state)
{
    bool low_power_profile = IS_ENABLED(CONFIG_UWB_TAG_LOW_POWER);

    if (ble_adv_policy_should_advertise(low_power_profile, state)) {
        ble_adv_enable();
    } else {
        ble_adv_disable();
    }
}
