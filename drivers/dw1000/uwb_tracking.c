/*! ----------------------------------------------------------------------------
 * @file    uwb_tracking.c
 * @brief   Tag-side per-cycle TDoA tracking (blinker) loop (UWB-252). See
 *          uwb_tracking.h for the full API doc, correctness-crux rationale,
 *          and on-hardware bring-up checklist.
 *
 * Pure composition -- every actual seam (SYNC RX, cycle reference, slot
 * timing, blink codec, delayed TX) is implemented elsewhere and unit-tested
 * in its own suite; this module only sequences them per the rules in
 * uwb_tracking.h.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/logging/log.h>

#include "dw1000_ranging.h"
#include "dw1000_sync.h"
#include "uwb_blink_codec.h"
#include "uwb_cycle_ref.h"
#include "uwb_frames.h"
#include "uwb_slot_timing.h"
#include "uwb_tracking.h"

LOG_MODULE_REGISTER(uwb_tracking, LOG_LEVEL_DBG);

/*
 * Blink TX scratch buffer: sized exactly to the PSDU (no FCS -- the DW1000
 * hardware appends 2 CRC bytes automatically on TX). dw1000_tx_at()'s 'len'
 * parameter follows deca_driver convention: the TOTAL frame length INCLUDING
 * those 2 CRC bytes (see dw1000_ranging.h and twr_responder.c's identical
 * convention).
 */
#define UWB_TRACKING_TX_BUF_SIZE  UWB_TAG_BLINK_FRAME_SIZE
#define UWB_TRACKING_CRC_LEN      2u

/* No UWB_BLINK_FLAG_* status bits are sourced by this module (e.g. battery
 * monitoring is a separate concern) -- every blink is built with flags = 0,
 * and uwb_build_tag_blink() masks reserved bits to 0 regardless. */
#define UWB_TRACKING_BLINK_FLAGS  0u

void uwb_tracking_state_init(uwb_tracking_state_t *state)
{
    if (state == NULL) {
        return;
    }

    uwb_cycle_ref_init(&state->cycle_ref);
    state->blink_count = 0;
    state->mac_seq = 0;
}

uwb_tracking_outcome_t uwb_tracking_run_cycle(const uwb_tracking_config_t *cfg,
                                               uwb_tracking_state_t *state,
                                               uint32_t sync_timeout_us)
{
    uwb_sync_info_t sync_info;
    uint64_t sync_rx_ts;
    uint16_t cycle_seq;
    uint64_t tx_time_dtu;
    uint8_t buf[UWB_TRACKING_TX_BUF_SIZE];
    int build_len;
    int ret;

    if (cfg == NULL || state == NULL) {
        LOG_ERR("uwb_tracking_run_cycle(): NULL cfg/state");
        return UWB_TRACKING_BUILD_ERROR;
    }

    /* ---- 1. Listen for this cycle's SYNC frame, update the reference ---- */
    ret = dw1000_sync_rx(&sync_info, sync_timeout_us);
    if (ret == 0) {
        uwb_cycle_ref_on_sync(&state->cycle_ref, &sync_info);
    } else {
        /* -ETIMEDOUT (nothing heard) / -EIO (RX error) / -EBADMSG (heard a
         * frame, but not a valid SYNC) are all "no SYNC this cycle" from the
         * cycle reference's point of view -- see uwb_cycle_ref_on_miss(). */
        LOG_DBG("No SYNC this cycle (%d)", ret);
        uwb_cycle_ref_on_miss(&state->cycle_ref);
    }

    /* ---- 2. Mandatory gate: never schedule a blink without a valid ref -- */
    if (!uwb_cycle_ref_get(&state->cycle_ref, &sync_rx_ts, &cycle_seq)) {
        LOG_DBG("No valid cycle reference -- not blinking this cycle");
        return UWB_TRACKING_NO_VALID_REF;
    }

    /* ---- 3. Compute this tag's scheduled blink TX time -------------------*/
    ret = uwb_slot_tx_time(sync_rx_ts, cfg->slot_idx, cfg->slot_duration_dtu,
                            cfg->slot_count, &tx_time_dtu);
    if (ret != 0) {
        LOG_ERR("uwb_slot_tx_time() rejected slot %u/%u (%d) -- not blinking "
                "this cycle",
                (unsigned)cfg->slot_idx, (unsigned)cfg->slot_count, ret);
        return UWB_TRACKING_BUILD_ERROR;
    }

    /* ---- 4. Build the blink, using the CURRENT (pre-increment) counter --*/
    build_len = uwb_build_tag_blink(buf, cfg->self_addr, state->mac_seq++,
                                     state->blink_count,
                                     UWB_TRACKING_BLINK_FLAGS);
    if (build_len < 0) {
        LOG_ERR("uwb_build_tag_blink() failed (%d) -- not blinking this cycle",
                build_len);
        return UWB_TRACKING_BUILD_ERROR;
    }

    /* ---- 5. Schedule the delayed TX --------------------------------------*/
    ret = dw1000_tx_at(buf, (uint16_t)(build_len + UWB_TRACKING_CRC_LEN),
                        tx_time_dtu, false);
    if (ret != 0) {
        /* Missed the scheduled slot-TX window (typically -EIO / HPDWARN --
         * the requested tx_time_dtu had already passed by the time
         * dwt_starttx() ran). Do NOT busy-retry within this call: the
         * scheduled instant is gone, and the peer anchors are not expecting
         * a transmission at an unplanned time. Also invalidate-vote the
         * cycle reference (uwb_cycle_ref.h) -- a late TX often means this
         * cycle's firmware processing fell behind, so re-syncing off a fresh
         * SYNC on a later cycle is safer than continuing to schedule off a
         * reference that may itself now be stale. */
        LOG_WRN("dw1000_tx_at() missed slot %u/%u (%d) -- dropping this "
                "cycle's blink",
                (unsigned)cfg->slot_idx, (unsigned)cfg->slot_count, ret);
        uwb_cycle_ref_on_miss(&state->cycle_ref);
        return UWB_TRACKING_MISSED_WINDOW;
    }

    LOG_DBG("Blink #%u transmitted in slot %u/%u (cycle_seq=0x%04X)",
            (unsigned)state->blink_count, (unsigned)cfg->slot_idx,
            (unsigned)cfg->slot_count, cycle_seq);

    /* Only increment on an ACTUALLY-transmitted blink -- never for a dropped
     * / missed-window cycle. */
    state->blink_count++;

    return UWB_TRACKING_BLINKED;
}
