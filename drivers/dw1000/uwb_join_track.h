/*! ----------------------------------------------------------------------------
 * @file    uwb_join_track.h
 * @brief   Orchestration seam: join (UWB-261) -> tracking (UWB-252) (UWB-263).
 *
 * ADR-0039 "Adopt" step + ADR-001 Sec Phase 2: a tag with no runtime
 * assignment yet runs the Aloha join state machine (uwb_join.h, UWB-261)
 * until it reaches JOINED, then converts the adopted uwb_join_result_t
 * (short_addr / slot_idx / slot_count / slot_duration_us) into the Phase-2
 * blinker's uwb_tracking_config_t (uwb_tracking.h, UWB-252) and runs the
 * per-cycle tracking loop with THAT config in place of any compile-time /
 * Kconfig-injected placeholder. This module is the seam that closes the
 * injected-config loop carried by UWB-9/250/251/252 -- it does not
 * reimplement either state machine, only sequences them.
 *
 * uwb_join_track_step() is a STEPPED function, exactly like uwb_join_step()
 * and uwb_tracking_run_cycle(): one call performs exactly one superframe
 * cycle's worth of work (either one join-phase step, or one tracking-phase
 * cycle) and always returns. Callers (e.g. samples/tag_join_track) drive the
 * "run forever" behaviour with their own `while (1)` loop.
 *
 * -----------------------------------------------------------------------
 * Phase transition (join -> track) and why it is one-way
 * -----------------------------------------------------------------------
 * uwb_join_track_state_t starts in UWB_JOIN_TRACK_PHASE_JOIN. Every step()
 * call drives uwb_join_step() until it returns UWB_JOIN_STEP_JOINED, at
 * which point this module:
 *
 *   1. Reads the adopted uwb_join_result_t (uwb_join_get_result()).
 *   2. Populates state->tracking_cfg from it (self_addr = short_addr,
 *      slot_idx / slot_count widened to uint32_t, slot_duration_us
 *      converted to DW1000 time units for slot_duration_dtu -- see
 *      uwb_join_track.c's us_to_slot_duration_dtu()).
 *   3. Initialises a fresh uwb_tracking_state_t (uwb_tracking_state_init()).
 *   4. Switches state->phase to UWB_JOIN_TRACK_PHASE_TRACK.
 *
 * and returns UWB_JOIN_TRACK_JOINED for that call -- no blink is attempted
 * the same cycle the assignment is adopted.
 *
 * Once in UWB_JOIN_TRACK_PHASE_TRACK, every subsequent step() call drives
 * uwb_tracking_run_cycle() and this module NEVER falls back to the JOIN
 * phase on its own -- not even after many consecutive
 * UWB_TRACKING_NO_VALID_REF cycles (transient SYNC loss). Per ADR-0039
 * ("Adopt" step): "A tag keeps its assignment across transient SYNC loss --
 * the cycle-reference gate (UWB-243) already suppresses blinks while SYNC is
 * absent; the tag re-joins only from a cold start / mode (re)entry." The
 * mandatory collision-avoidance gate inside uwb_tracking_run_cycle() itself
 * (see uwb_tracking.h) is what keeps a sync-lost tag silent, NOT a return to
 * the join phase. The only way back to UWB_JOIN_TRACK_PHASE_JOIN is a fresh
 * uwb_join_track_state_init() call (a cold start / explicit mode re-entry --
 * out of scope here, see "Out of scope" below).
 *
 * A tag that never receives a matching SLOT_ASSIGNMENT never leaves
 * UWB_JOIN_TRACK_PHASE_JOIN (uwb_join_step()'s own give-up-and-restart /
 * backoff handles the "extended failure to join" case entirely -- this
 * module adds no additional retry/timeout logic of its own), and therefore
 * uwb_tracking_run_cycle() -- and hence the blink dw1000_tx_at() call -- is
 * never reached.
 *
 * -----------------------------------------------------------------------
 * Out of scope (see UWB-263 ticket and sibling subissues)
 * -----------------------------------------------------------------------
 *   - The join state machine itself (UWB-261) and the master-side registrar
 *     (anchor repo, UWB-260/262) -- consumed here, not reimplemented.
 *   - The two-mode calibration <-> tracking orchestration (a different mode
 *     switch than join -> track; separate follow-up).
 *   - Publishing the EUI-64 <-> short_addr binding to the cloud.
 *   - Any contracts/uwb wire-format change (v0.6 stays pinned).
 *   - Re-entering the join phase after a cold reset / explicit mode restart
 *     -- callers achieve that today simply by calling
 *     uwb_join_track_state_init() again (e.g. on their own reset path); no
 *     dedicated API is added here because nothing in this ticket's scope
 *     drives that trigger yet.
 *
 * -----------------------------------------------------------------------
 * On-hardware bring-up (manual -- not automated by the ztest suite)
 * -----------------------------------------------------------------------
 * See samples/tag_join_track/README.md for the full two-board bring-up
 * checklist (one anchor registrar-master + SYNC, one tag running this
 * seam via the sample) -- this header only notes the seam-specific pass
 * criteria: the tag transmits NO frame at all until it has TXed a
 * JOIN_REQUEST (join phase), and TXes ONLY in its adopted slot once
 * tracking (never off a hard-coded/injected slot config).
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef UWB_JOIN_TRACK_H_
#define UWB_JOIN_TRACK_H_

#include <stdbool.h>
#include <stdint.h>

#include "uwb_join.h"
#include "uwb_tracking.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caller-injected configuration for the join -> track seam.
 */
typedef struct {
    uwb_join_config_t join;  /**< Forwarded verbatim to uwb_join_step() while
                                   in the join phase -- see uwb_join.h. */

    uint32_t tracking_sync_timeout_us;  /**< RX timeout forwarded to
                                              uwb_tracking_run_cycle() once
                                              the track phase is entered --
                                              see uwb_tracking.h. May differ
                                              from join.sync_timeout_us /
                                              join.assignment_timeout_us
                                              (different phases, potentially
                                              different superframe listen
                                              windows). */
} uwb_join_track_config_t;

/**
 * @brief Which state machine is currently driving uwb_join_track_step().
 */
typedef enum {
    UWB_JOIN_TRACK_PHASE_JOIN = 0,  /**< Driving uwb_join_step(); no runtime
                                          assignment adopted yet. */
    UWB_JOIN_TRACK_PHASE_TRACK,      /**< Driving uwb_tracking_run_cycle()
                                          with the adopted config. Terminal
                                          from this module's point of view --
                                          see this header's top-of-file
                                          "Phase transition" section. */
} uwb_join_track_phase_t;

/**
 * @brief Persistent join+track state, carried across uwb_join_track_step()
 *        calls.
 *
 * Must be initialised via uwb_join_track_state_init() before the first
 * uwb_join_track_step() call.
 */
typedef struct {
    uwb_join_track_phase_t phase;
    uwb_join_state_t       join_state;      /**< Driven while phase ==
                                                  UWB_JOIN_TRACK_PHASE_JOIN. */
    uwb_tracking_config_t  tracking_cfg;     /**< Populated from the adopted
                                                  uwb_join_result_t on the
                                                  join -> track transition;
                                                  meaningless before that. */
    uwb_tracking_state_t   tracking_state;   /**< Driven while phase ==
                                                  UWB_JOIN_TRACK_PHASE_TRACK. */
} uwb_join_track_state_t;

/**
 * @brief Outcome of one uwb_join_track_step() call.
 *
 * The JOIN_* values mirror uwb_join_step_outcome_t (minus
 * UWB_JOIN_STEP_ALREADY_JOINED, which cannot occur here -- this module
 * transitions phase away from JOIN the same call it observes
 * UWB_JOIN_STEP_JOINED) and are only returned while
 * @c state->phase == UWB_JOIN_TRACK_PHASE_JOIN. The tracking-mirroring
 * values are only returned once @c state->phase == UWB_JOIN_TRACK_PHASE_TRACK.
 */
typedef enum {
    /* ---- Join phase (mirrors uwb_join_step_outcome_t) ---- */
    UWB_JOIN_TRACK_JOIN_GATE_LISTENING,
    UWB_JOIN_TRACK_JOIN_GATE_SYNC_HEARD,
    UWB_JOIN_TRACK_JOIN_REQUEST_SENT,
    UWB_JOIN_TRACK_JOIN_REQUEST_TX_ERROR,
    UWB_JOIN_TRACK_JOIN_AWAIT_TIMEOUT,
    UWB_JOIN_TRACK_JOIN_AWAIT_FOREIGN,
    UWB_JOIN_TRACK_JOIN_BACKOFF_WAITING,

    /** Adopted a matching SLOT_ASSIGNMENT THIS call and transitioned to the
     *  track phase (see this header's "Phase transition" section). No blink
     *  is attempted the same call -- the next uwb_join_track_step() call
     *  runs the first tracking cycle. */
    UWB_JOIN_TRACK_JOINED,

    /* ---- Track phase (mirrors uwb_tracking_outcome_t) ---- */
    UWB_JOIN_TRACK_BLINKED,
    UWB_JOIN_TRACK_NO_VALID_REF,
    UWB_JOIN_TRACK_MISSED_WINDOW,
    UWB_JOIN_TRACK_BUILD_ERROR,

    /** NULL cfg/state, or a required injected join callback is NULL (see
     *  uwb_join_step_outcome_t's UWB_JOIN_STEP_INVALID_ARGS) -- no radio
     *  touched. Can occur regardless of @c state->phase. */
    UWB_JOIN_TRACK_INVALID_ARGS,
} uwb_join_track_outcome_t;

/**
 * @brief Initialise a join+track state machine, starting in the join phase.
 *
 * @param state  State to initialise. No-op if NULL.
 * @param cfg    Configuration to read @c cfg->join from (forwarded to
 *                 uwb_join_state_init()). May be NULL -- see
 *                 uwb_join_state_init()'s own NULL handling; callers MUST
 *                 then call uwb_join_track_step() and check for
 *                 UWB_JOIN_TRACK_INVALID_ARGS rather than relying on
 *                 @p state's contents.
 */
void uwb_join_track_state_init(uwb_join_track_state_t *state,
                                const uwb_join_track_config_t *cfg);

/**
 * @brief Advance the join+track seam by exactly one superframe cycle.
 *
 * While @c state->phase == UWB_JOIN_TRACK_PHASE_JOIN, forwards to
 * uwb_join_step(&cfg->join, &state->join_state); on
 * UWB_JOIN_STEP_JOINED, performs the join -> track transition described in
 * this header's top-of-file comment and returns UWB_JOIN_TRACK_JOINED.
 *
 * While @c state->phase == UWB_JOIN_TRACK_PHASE_TRACK, forwards to
 * uwb_tracking_run_cycle(&state->tracking_cfg, &state->tracking_state,
 * cfg->tracking_sync_timeout_us) and maps its outcome 1:1.
 *
 * Never blocks beyond the bounded RX timeouts the underlying state machine
 * uses for the current phase -- always returns, so callers drive the
 * "join-then-track forever" loop with their own `while (1)` calling this
 * once per cycle (see samples/tag_join_track/src/main.c).
 *
 * @param cfg    Configuration. Must not be NULL; cfg->join's required
 *                 injected callbacks (eui64_get / rng / now_get) must not be
 *                 NULL while in the join phase (uwb_join_step() itself
 *                 validates this).
 * @param state  Persistent state, previously initialised by
 *                 uwb_join_track_state_init(). Must not be NULL.
 * @return  See uwb_join_track_outcome_t. UWB_JOIN_TRACK_INVALID_ARGS on NULL
 *          @p cfg / @p state without touching the radio.
 */
uwb_join_track_outcome_t uwb_join_track_step(const uwb_join_track_config_t *cfg,
                                              uwb_join_track_state_t *state);

/**
 * @brief Convenience predicate: has the seam adopted an assignment and
 *        entered the track phase?
 *
 * @param state  State to read. NULL is treated as "not tracking".
 * @return  true iff @c state->phase == UWB_JOIN_TRACK_PHASE_TRACK.
 */
bool uwb_join_track_is_tracking(const uwb_join_track_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* UWB_JOIN_TRACK_H_ */
