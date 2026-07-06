/*! ----------------------------------------------------------------------------
 * @file    uwb_cycle_ref.c
 * @brief   Tag-side maintained cycle reference (UWB-243). See uwb_cycle_ref.h
 *          for the full API doc, scope note, and the UWB-9 seam contract.
 *
 * Pure logic -- no deca_driver, no radio, no timers. Every function only
 * touches the uwb_cycle_ref_t struct passed in and (for the discontinuity
 * warning) the logging subsystem.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/logging/log.h>

#include "uwb_cycle_ref.h"

LOG_MODULE_REGISTER(uwb_cycle_ref, LOG_LEVEL_DBG);

void uwb_cycle_ref_init(uwb_cycle_ref_t *r)
{
    if (r == NULL) {
        return;
    }

    r->valid = false;
    r->missed = 0;
}

void uwb_cycle_ref_on_sync(uwb_cycle_ref_t *r, const uwb_sync_info_t *s)
{
    if (r == NULL || s == NULL) {
        return;
    }

    if (r->valid) {
        /* uint16_t arithmetic wraps at 2^16 automatically, matching the
         * cycle_seq wire field's documented wraparound (uwb_frames.h). */
        uint16_t expected = (uint16_t)(r->cycle_seq + 1u);

        if (s->cycle_seq != expected) {
            LOG_WRN("cycle_seq discontinuity: expected 0x%04X, got 0x%04X "
                     "(re-anchoring anyway)",
                     expected, s->cycle_seq);
        }
    }

    r->sync_rx_ts = s->rx_ts;
    r->cycle_seq = s->cycle_seq;
    r->valid = true;
    r->missed = 0;
}

void uwb_cycle_ref_on_miss(uwb_cycle_ref_t *r)
{
    if (r == NULL) {
        return;
    }

    r->missed++;

    if (r->missed > CONFIG_UWB_SYNC_MAX_MISSED) {
        r->valid = false;
    }
}

bool uwb_cycle_ref_get(const uwb_cycle_ref_t *r, uint64_t *sync_rx_ts,
                       uint16_t *cycle_seq)
{
    if (r == NULL || !r->valid) {
        return false;
    }

    if (sync_rx_ts != NULL) {
        *sync_rx_ts = r->sync_rx_ts;
    }

    if (cycle_seq != NULL) {
        *cycle_seq = r->cycle_seq;
    }

    return true;
}
