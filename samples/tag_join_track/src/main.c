/*! ----------------------------------------------------------------------------
 * @file    main.c
 * @brief   Join -> tracking sample (UWB-263)
 *
 * Runs dw1000_port_init() -> dw1000_configure() -> repeatedly calls
 * uwb_join_track_step() (drivers/dw1000/uwb_join_track.h) once per
 * superframe cycle, logging join attempts/backoff while unassigned, the
 * adopted SLOT_ASSIGNMENT once joined, and every tracking cycle's blink
 * outcome afterwards.
 *
 * Unlike samples/tag_tracking (UWB-252), this sample injects NO slot
 * config -- self_addr / slot_idx / slot_count / slot_duration_dtu come
 * ENTIRELY from the SLOT_ASSIGNMENT this tag receives at runtime (ADR-0039).
 * If this tag never receives a matching assignment, it never leaves the join
 * phase and never transmits a blink.
 *
 * The three injected uwb_join_config_t callbacks are wired to real hardware
 * here (deliberately NOT done by uwb_join.c itself -- see uwb_join.h's
 * "Injected seams" section):
 *   - eui64_get  -> dwt_geteui() (this board's real DW1000 EUI-64 register).
 *   - now_get    -> dwt_readsystimestamphi32() << 8 (the DW1000 40-bit system
 *                    time, exactly the reference dw1000_ranging.h's bring-up
 *                    note suggests for scheduling a delayed TX from "now").
 *   - rng        -> Zephyr's sys_rand32_get() (<zephyr/random/random.h>),
 *                    reduced to a uniform value in [min, max].
 *
 * NOTE: This sample requires an on-target run with a real DWM1001 module
 *       hearing SYNC frames + responding to a JOIN_REQUEST from a
 *       clock-master anchor running the ADR-0039 master-side registrar (the
 *       anchor repo, UWB-260/262) -- see this directory's README.md. For
 *       headless CI verification see tests/join_track/ (ztest, mocked
 *       deca_driver).
 *
 * Build:   west build -b nrf52dk_nrf52832 samples/tag_join_track
 * Flash:   west flash
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "deca_port.h"
#include "dw1000_config.h"
#include "uwb_frames.h"
#include "uwb_join.h"
#include "uwb_join_codec.h"
#include "uwb_join_track.h"
#include "uwb_tracking.h"

LOG_MODULE_REGISTER(tag_join_track_sample, LOG_LEVEL_DBG);

/* ---------------------------------------------------------------------------
 * Injected join-config callbacks -- real hardware wiring (see this file's
 * top-of-file comment).
 * --------------------------------------------------------------------------- */

static void real_eui64_get(uint8_t eui64_out[UWB_EUI64_LEN])
{
    dwt_geteui((uint8 *)eui64_out);
}

static uint64_t real_now_get(void)
{
    /* dw1000_ranging.h's bring-up note: dwt_readsystimestamphi32() << 8 is
     * the documented way to obtain a 40-bit-range "now" reference for
     * dw1000_delayed_tx_time() outside of an RX event (which would otherwise
     * supply the reference via its own timestamp). */
    return (uint64_t)dwt_readsystimestamphi32() << 8;
}

static uint32_t real_rng(uint32_t min, uint32_t max)
{
    uint32_t span;

    if (max <= min) {
        return min;
    }

    span = max - min + 1u; /* inclusive range */
    if (span == 0u) {
        /* max - min + 1 wrapped to 0 only when [min, max] spans the entire
         * uint32_t range -- any draw is valid then. */
        return sys_rand32_get();
    }

    return min + (sys_rand32_get() % span);
}

/* ---------------------------------------------------------------------------
 * Outcome -> string logging helpers
 * --------------------------------------------------------------------------- */

static const char *outcome_str(uwb_join_track_outcome_t outcome)
{
    switch (outcome) {
    case UWB_JOIN_TRACK_JOIN_GATE_LISTENING:
        return "JOIN_GATE_LISTENING";
    case UWB_JOIN_TRACK_JOIN_GATE_SYNC_HEARD:
        return "JOIN_GATE_SYNC_HEARD";
    case UWB_JOIN_TRACK_JOIN_REQUEST_SENT:
        return "JOIN_REQUEST_SENT";
    case UWB_JOIN_TRACK_JOIN_REQUEST_TX_ERROR:
        return "JOIN_REQUEST_TX_ERROR";
    case UWB_JOIN_TRACK_JOIN_AWAIT_TIMEOUT:
        return "JOIN_AWAIT_TIMEOUT";
    case UWB_JOIN_TRACK_JOIN_AWAIT_FOREIGN:
        return "JOIN_AWAIT_FOREIGN";
    case UWB_JOIN_TRACK_JOIN_BACKOFF_WAITING:
        return "JOIN_BACKOFF_WAITING";
    case UWB_JOIN_TRACK_JOINED:
        return "JOINED";
    case UWB_JOIN_TRACK_BLINKED:
        return "BLINKED";
    case UWB_JOIN_TRACK_NO_VALID_REF:
        return "NO_VALID_REF";
    case UWB_JOIN_TRACK_MISSED_WINDOW:
        return "MISSED_WINDOW";
    case UWB_JOIN_TRACK_BUILD_ERROR:
        return "BUILD_ERROR";
    case UWB_JOIN_TRACK_INVALID_ARGS:
        return "INVALID_ARGS";
    default:
        return "UNKNOWN";
    }
}

int main(void)
{
    int ret;
    uwb_join_track_config_t cfg = {0};
    uwb_join_track_state_t state;
    uint32_t cycle = 0;

    cfg.join.eui64_get = real_eui64_get;
    cfg.join.rng = real_rng;
    cfg.join.now_get = real_now_get;
    cfg.join.capabilities =
        IS_ENABLED(CONFIG_TAG_JOIN_TRACK_SAMPLE_REFERENCE_ROLE) ? UWB_JOIN_CAP_ROLE_REFERENCE : 0;
    cfg.join.gate_cycles = (uint32_t)CONFIG_TAG_JOIN_TRACK_SAMPLE_GATE_CYCLES;
    cfg.join.await_cycles = (uint32_t)CONFIG_TAG_JOIN_TRACK_SAMPLE_AWAIT_CYCLES;
    cfg.join.backoff_base_cycles = (uint32_t)CONFIG_TAG_JOIN_TRACK_SAMPLE_BACKOFF_BASE_CYCLES;
    cfg.join.backoff_max_cycles = (uint32_t)CONFIG_TAG_JOIN_TRACK_SAMPLE_BACKOFF_MAX_CYCLES;
    cfg.join.max_attempts = (uint32_t)CONFIG_TAG_JOIN_TRACK_SAMPLE_MAX_ATTEMPTS;
    cfg.join.sync_timeout_us = (uint32_t)CONFIG_TAG_JOIN_TRACK_SAMPLE_SYNC_TIMEOUT_US;
    cfg.join.assignment_timeout_us =
        (uint32_t)CONFIG_TAG_JOIN_TRACK_SAMPLE_ASSIGNMENT_TIMEOUT_US;
    cfg.join.tx_margin_dtu = (uint32_t)CONFIG_TAG_JOIN_TRACK_SAMPLE_TX_MARGIN_DTU;

    cfg.tracking_sync_timeout_us = (uint32_t)CONFIG_TAG_JOIN_TRACK_SAMPLE_SYNC_TIMEOUT_US;

    LOG_INF("Join+track sample starting -- capabilities=0x%02X, gate_cycles=%u, "
            "await_cycles=%u, backoff=[%u,%u], max_attempts=%u",
            cfg.join.capabilities, (unsigned)cfg.join.gate_cycles,
            (unsigned)cfg.join.await_cycles, (unsigned)cfg.join.backoff_base_cycles,
            (unsigned)cfg.join.backoff_max_cycles, (unsigned)cfg.join.max_attempts);

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

    LOG_INF("dw1000_configure() OK -- entering join phase (NO injected slot "
            "config -- address/slot come only from the adopted "
            "SLOT_ASSIGNMENT)");

    uwb_join_track_state_init(&state, &cfg);

    while (1) {
        bool was_tracking = uwb_join_track_is_tracking(&state);
        uwb_join_track_outcome_t outcome = uwb_join_track_step(&cfg, &state);

        cycle++;

        if (outcome == UWB_JOIN_TRACK_JOINED) {
            LOG_INF("cycle %u: JOINED -- adopted short_addr=0x%04X slot=%u/%u "
                    "slot_duration_dtu=%u -- entering track phase",
                    (unsigned)cycle, state.tracking_cfg.self_addr,
                    (unsigned)state.tracking_cfg.slot_idx,
                    (unsigned)state.tracking_cfg.slot_count,
                    (unsigned)state.tracking_cfg.slot_duration_dtu);
        } else if (!was_tracking) {
            /* Still in the join phase -- log attempts/backoff at INF so
             * bring-up progress is visible without bumping the whole board
             * to DBG. */
            LOG_INF("cycle %u: %s (still joining)", (unsigned)cycle,
                    outcome_str(outcome));
        } else if (outcome == UWB_JOIN_TRACK_BLINKED) {
            LOG_INF("cycle %u: blink_count=%u: %s", (unsigned)cycle,
                    (unsigned)state.tracking_state.blink_count, outcome_str(outcome));
        } else {
            LOG_WRN("cycle %u: %s", (unsigned)cycle, outcome_str(outcome));
        }
    }

    /* Unreachable; main() returning is treated as an error in Zephyr. */
    return 0;
}
