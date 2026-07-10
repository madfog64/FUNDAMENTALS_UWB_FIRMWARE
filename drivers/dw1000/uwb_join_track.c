/*! ----------------------------------------------------------------------------
 * @file    uwb_join_track.c
 * @brief   Orchestration seam: join (UWB-261) -> tracking (UWB-252) (UWB-263).
 *          See uwb_join_track.h for the full API doc, phase-transition
 *          rationale, and on-hardware bring-up pointer.
 *
 * Pure composition -- every actual seam (the join state machine, the
 * tracking blinker loop) is implemented and unit-tested elsewhere; this
 * module only sequences them plus the join_result -> tracking_config
 * conversion documented below.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include "uwb_join.h"
#include "uwb_join_track.h"
#include "uwb_tracking.h"

LOG_MODULE_REGISTER(uwb_join_track, LOG_LEVEL_DBG);

/*
 * Convert the join-adopted SLOT_ASSIGNMENT's slot_duration_us (contracts/uwb
 * v0.6, microseconds) into DW1000 time units (DTU) for
 * uwb_tracking_config_t::slot_duration_dtu (uwb_slot_timing.h).
 *
 * 1 DTU = 1 / (128 x 499.2 MHz) s (deca_device_api.h's DWT_TIME_UNITS), so
 * 1 us = 128 x 499.2 = 63897.6 DTU -- a non-integer ratio. Computed here as
 * (us * 638976) / 10 to stay in exact 64-bit integer arithmetic (matches the
 * ratio to one decimal place). The sub-DTU remainder this drops is
 * irrelevant: uwb_slot_timing.h is explicit that TDoA collision avoidance
 * needs slot-level accuracy ("land inside the assigned slot, clear of guard
 * bands"), not nanosecond precision -- the guard band folded into
 * slot_duration_dtu absorbs far more than one DTU (~15.65 ps) of rounding.
 */
static uint32_t us_to_slot_duration_dtu(uint16_t slot_duration_us)
{
    uint64_t dtu = ((uint64_t)slot_duration_us * 638976ULL) / 10ULL;

    /* Defensive clamp: slot_duration_us is 16-bit (max 65535), so
     * dtu is already just under UINT32_MAX (~4.19e9 vs ~4.29e9) and cannot
     * realistically overflow -- clamp rather than silently truncate on a
     * 32-bit assignment if that ever changes. */
    if (dtu > (uint64_t)UINT32_MAX) {
        dtu = (uint64_t)UINT32_MAX;
    }

    return (uint32_t)dtu;
}

/*
 * Perform the join -> track transition: read the adopted uwb_join_result_t,
 * populate state->tracking_cfg from it, start a fresh tracking state, and
 * flip the phase. Called exactly once, the same uwb_join_track_step() call
 * that observes UWB_JOIN_STEP_JOINED.
 */
static void adopt_and_enter_track_phase(uwb_join_track_state_t *state)
{
    uwb_join_result_t result;

    if (!uwb_join_get_result(&state->join_state, &result)) {
        /* Defensive only -- uwb_join_step() just returned
         * UWB_JOIN_STEP_JOINED, so uwb_join_get_result() reporting false
         * here would indicate a uwb_join.c bug, not a condition this seam
         * needs its own recovery path for. Stay in the join phase rather
         * than adopt a garbage config. */
        LOG_ERR("uwb_join_step() reported JOINED but uwb_join_get_result() "
                "returned false -- staying in the join phase");
        return;
    }

    state->tracking_cfg.self_addr = result.short_addr;
    state->tracking_cfg.slot_idx = (uint32_t)result.slot_idx;
    state->tracking_cfg.slot_count = (uint32_t)result.slot_count;
    state->tracking_cfg.slot_duration_dtu =
        us_to_slot_duration_dtu(result.slot_duration_us);

    uwb_tracking_state_init(&state->tracking_state);
    state->phase = UWB_JOIN_TRACK_PHASE_TRACK;

    LOG_INF("Adopted assignment -- entering track phase: short_addr=0x%04X "
            "slot=%u/%u slot_duration_us=%u (slot_duration_dtu=%u)",
            result.short_addr, (unsigned)result.slot_idx,
            (unsigned)result.slot_count, (unsigned)result.slot_duration_us,
            (unsigned)state->tracking_cfg.slot_duration_dtu);
}

static uwb_join_track_outcome_t map_join_outcome(uwb_join_step_outcome_t outcome)
{
    switch (outcome) {
    case UWB_JOIN_STEP_GATE_LISTENING:
        return UWB_JOIN_TRACK_JOIN_GATE_LISTENING;
    case UWB_JOIN_STEP_GATE_SYNC_HEARD:
        return UWB_JOIN_TRACK_JOIN_GATE_SYNC_HEARD;
    case UWB_JOIN_STEP_REQUEST_SENT:
        return UWB_JOIN_TRACK_JOIN_REQUEST_SENT;
    case UWB_JOIN_STEP_REQUEST_TX_ERROR:
        return UWB_JOIN_TRACK_JOIN_REQUEST_TX_ERROR;
    case UWB_JOIN_STEP_AWAIT_TIMEOUT:
        return UWB_JOIN_TRACK_JOIN_AWAIT_TIMEOUT;
    case UWB_JOIN_STEP_AWAIT_FOREIGN:
        return UWB_JOIN_TRACK_JOIN_AWAIT_FOREIGN;
    case UWB_JOIN_STEP_BACKOFF_WAITING:
        return UWB_JOIN_TRACK_JOIN_BACKOFF_WAITING;
    case UWB_JOIN_STEP_INVALID_ARGS:
        return UWB_JOIN_TRACK_INVALID_ARGS;
    case UWB_JOIN_STEP_JOINED:
    case UWB_JOIN_STEP_ALREADY_JOINED:
    default:
        /* JOINED is handled specially by the caller before this mapping is
         * consulted; ALREADY_JOINED cannot occur while this seam's phase is
         * still JOIN (uwb_join_step() only returns it after a PRIOR JOINED,
         * and this seam transitions phase away from JOIN the same call it
         * sees JOINED -- see uwb_join_track_step()). Both landing here would
         * indicate a state-machine bug, not a condition to recover from
         * silently. */
        LOG_ERR("uwb_join_step() returned unexpected outcome %d while in "
                "the join phase",
                (int)outcome);
        return UWB_JOIN_TRACK_INVALID_ARGS;
    }
}

static uwb_join_track_outcome_t map_tracking_outcome(uwb_tracking_outcome_t outcome)
{
    switch (outcome) {
    case UWB_TRACKING_BLINKED:
        return UWB_JOIN_TRACK_BLINKED;
    case UWB_TRACKING_NO_VALID_REF:
        return UWB_JOIN_TRACK_NO_VALID_REF;
    case UWB_TRACKING_MISSED_WINDOW:
        return UWB_JOIN_TRACK_MISSED_WINDOW;
    case UWB_TRACKING_BUILD_ERROR:
    default:
        return UWB_JOIN_TRACK_BUILD_ERROR;
    }
}

void uwb_join_track_state_init(uwb_join_track_state_t *state,
                                const uwb_join_track_config_t *cfg)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->phase = UWB_JOIN_TRACK_PHASE_JOIN;

    uwb_join_state_init(&state->join_state, cfg != NULL ? &cfg->join : NULL);
}

uwb_join_track_outcome_t uwb_join_track_step(const uwb_join_track_config_t *cfg,
                                              uwb_join_track_state_t *state)
{
    if (cfg == NULL || state == NULL) {
        LOG_ERR("uwb_join_track_step(): NULL cfg/state");
        return UWB_JOIN_TRACK_INVALID_ARGS;
    }

    if (state->phase == UWB_JOIN_TRACK_PHASE_JOIN) {
        uwb_join_step_outcome_t join_outcome =
            uwb_join_step(&cfg->join, &state->join_state);

        if (join_outcome == UWB_JOIN_STEP_JOINED) {
            adopt_and_enter_track_phase(state);

            /* adopt_and_enter_track_phase() defensively stays in the join
             * phase if uwb_join_get_result() unexpectedly failed (see its
             * comment) -- only report JOINED if the transition actually
             * happened. */
            if (state->phase == UWB_JOIN_TRACK_PHASE_TRACK) {
                return UWB_JOIN_TRACK_JOINED;
            }

            return UWB_JOIN_TRACK_INVALID_ARGS;
        }

        return map_join_outcome(join_outcome);
    }

    /* UWB_JOIN_TRACK_PHASE_TRACK: per ADR-0039 ("Adopt"), an already-joined
     * tag keeps its assignment across transient SYNC loss and never falls
     * back to the join phase from here -- see uwb_join_track.h's
     * "Phase transition" section for the full rationale. */
    uwb_tracking_outcome_t tracking_outcome = uwb_tracking_run_cycle(
        &state->tracking_cfg, &state->tracking_state, cfg->tracking_sync_timeout_us);

    return map_tracking_outcome(tracking_outcome);
}

bool uwb_join_track_is_tracking(const uwb_join_track_state_t *state)
{
    return state != NULL && state->phase == UWB_JOIN_TRACK_PHASE_TRACK;
}
