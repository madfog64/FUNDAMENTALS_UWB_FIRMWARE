/*! ----------------------------------------------------------------------------
 * @file    main.c
 * @brief   TDoA tag tracking (blinker) loop sample (UWB-252)
 *
 * Runs dw1000_port_init() -> dw1000_configure() -> repeatedly calls
 * uwb_tracking_run_cycle() once per superframe cycle with this device's
 * placeholder tracking configuration (CONFIG_TAG_TRACKING_SAMPLE_* -- real
 * slot/address assignment is the Aloha join/registration flow, UWB-9/10/11,
 * out of scope here), logging the outcome of every cycle.
 *
 * NOTE: This sample requires an on-target run with a real DWM1001 module
 *       hearing SYNC frames from a clock-master anchor (or a second DWM1001
 *       standing in for one, see drivers/dw1000/uwb_tracking.h's on-hardware
 *       bring-up checklist) -- see this directory's README.md. For headless
 *       CI verification see tests/tracking/ (ztest).
 *
 * Build:   west build -b nrf52dk_nrf52832 samples/tag_tracking
 * Flash:   west flash
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "deca_port.h"
#include "dw1000_config.h"
#include "uwb_cycle_ref.h"
#include "uwb_tracking.h"

LOG_MODULE_REGISTER(tag_tracking_sample, LOG_LEVEL_DBG);

static const char *outcome_str(uwb_tracking_outcome_t outcome)
{
    switch (outcome) {
    case UWB_TRACKING_BLINKED:
        return "BLINKED";
    case UWB_TRACKING_NO_VALID_REF:
        return "NO_VALID_REF";
    case UWB_TRACKING_MISSED_WINDOW:
        return "MISSED_WINDOW";
    case UWB_TRACKING_BUILD_ERROR:
        return "BUILD_ERROR";
    default:
        return "UNKNOWN";
    }
}

int main(void)
{
    int ret;
    const uwb_tracking_config_t cfg = {
        .self_addr = (uint16_t)CONFIG_TAG_TRACKING_SAMPLE_SELF_ADDR,
        .slot_idx = (uint32_t)CONFIG_TAG_TRACKING_SAMPLE_SLOT_IDX,
        .slot_count = (uint32_t)CONFIG_TAG_TRACKING_SAMPLE_SLOT_COUNT,
        .slot_duration_dtu = (uint32_t)CONFIG_TAG_TRACKING_SAMPLE_SLOT_DURATION_DTU,
    };
    uwb_tracking_state_t state;

    LOG_INF("Tag tracking sample starting -- self_addr = 0x%04X, slot %u/%u, "
            "slot_duration_dtu = %u",
            cfg.self_addr, (unsigned)cfg.slot_idx, (unsigned)cfg.slot_count,
            (unsigned)cfg.slot_duration_dtu);

    ret = dw1000_port_init();
    if (ret < 0) {
        LOG_ERR("dw1000_port_init failed: %d", ret);
        return ret;
    }

    ret = dw1000_configure();
    if (ret < 0) {
        LOG_ERR("dw1000_configure failed: %d -- check SPI wiring", ret);
        return ret;
    }

    LOG_INF("dw1000_configure() OK -- entering tag tracking (blinker) loop");

    uwb_tracking_state_init(&state);

    while (1) {
        uwb_tracking_outcome_t outcome = uwb_tracking_run_cycle(
            &cfg, &state,
            (uint32_t)CONFIG_TAG_TRACKING_SAMPLE_SYNC_TIMEOUT_US);

        uint64_t sync_rx_ts;
        uint16_t cycle_seq = 0;
        bool ref_valid =
            uwb_cycle_ref_get(&state.cycle_ref, &sync_rx_ts, &cycle_seq);

        if (outcome == UWB_TRACKING_BLINKED) {
            LOG_INF("cycle_seq=0x%04X blink_count=%u: %s", cycle_seq,
                    (unsigned)state.blink_count, outcome_str(outcome));
        } else if (ref_valid) {
            /* Reference is valid (still within CONFIG_UWB_SYNC_MAX_MISSED
             * tolerance) but this cycle did not produce a transmitted blink
             * (UWB_TRACKING_MISSED_WINDOW / UWB_TRACKING_BUILD_ERROR). */
            LOG_WRN("cycle_seq=0x%04X blink_count=%u: %s", cycle_seq,
                    (unsigned)state.blink_count, outcome_str(outcome));
        } else {
            /* UWB_TRACKING_NO_VALID_REF -- never synced, or sync lost. */
            LOG_DBG("blink_count=%u: %s (no valid cycle reference)",
                    (unsigned)state.blink_count, outcome_str(outcome));
        }
    }

    /* Unreachable; main() returning is treated as an error in Zephyr. */
    return 0;
}
