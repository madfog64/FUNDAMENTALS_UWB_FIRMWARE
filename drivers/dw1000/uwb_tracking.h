/*! ----------------------------------------------------------------------------
 * @file    uwb_tracking.h
 * @brief   Tag-side per-cycle TDoA tracking (blinker) loop (UWB-252).
 *
 * ADR-001 §Phase 2: in TDoA tracking every superframe cycle a tag listens for
 * the master anchor's SYNC frame, updates its cycle reference, and -- only if
 * that reference is currently valid -- builds and schedules its blink in its
 * registration-assigned TDMA slot. This module is the top-level composition
 * that wires the previously independent seams together into that per-cycle
 * behaviour:
 *
 *   dw1000_sync_rx()      (UWB-242) -- hear + validate/parse the SYNC frame.
 *   uwb_cycle_ref_*()      (UWB-243) -- maintain the cycle-start reference;
 *                            THE mandatory validity gate before scheduling a
 *                            blink (see "Correctness crux" below).
 *   uwb_slot_tx_time()     (UWB-251) -- compute this tag's scheduled blink TX
 *                            time from the cycle reference + slot config.
 *   uwb_build_tag_blink()   (UWB-250) -- build the blink PSDU.
 *   dw1000_tx_at()          (UWB-231) -- schedule the delayed TX.
 *
 * uwb_tracking_run_cycle() is a STEPPED function: one call performs exactly
 * one superframe cycle's worth of work (listen once, maybe blink once) and
 * always returns -- it never loops or blocks beyond the bounded RX timeout
 * passed in. Callers (e.g. samples/tag_tracking) drive the "run forever"
 * behaviour with their own `while (1)` loop, exactly as UWB-232's
 * twr_responder_run_once() pattern already established for the responder
 * side.
 *
 * -----------------------------------------------------------------------
 * Correctness crux: the reference-validity gate
 * -----------------------------------------------------------------------
 * TDoA cancels the tag's unknown TX time -- collision avoidance is entirely a
 * function of the tag's blink landing inside its assigned slot, which in turn
 * depends on the tag having a CURRENT cycle-start reference (uwb_cycle_ref.h).
 * uwb_tracking_run_cycle() therefore:
 *
 *   1. ALWAYS updates the cycle reference first (uwb_cycle_ref_on_sync() on a
 *      successful dw1000_sync_rx(), uwb_cycle_ref_on_miss() on any failure --
 *      timeout, RX error, or a heard-but-invalid frame are all "no SYNC this
 *      cycle" from the reference's point of view).
 *   2. ONLY THEN reads the reference via uwb_cycle_ref_get(). If that reports
 *      invalid (never synced, or sync lost per CONFIG_UWB_SYNC_MAX_MISSED),
 *      this function returns UWB_TRACKING_NO_VALID_REF WITHOUT calling
 *      uwb_slot_tx_time() or dw1000_tx_at() -- there is no trustworthy epoch
 *      to schedule a blink against, and blinking anyway risks colliding with
 *      another tag's slot in a superframe this tag can no longer see the true
 *      start of.
 *
 * A missed slot-TX window (dw1000_tx_at() returns -EIO -- the scheduled time
 * had already passed by the time the driver got there) is handled the same
 * way twr_responder.c handles a missed RESPONSE/FINAL slot: the blink for
 * this cycle is dropped (NOT retried within the same call -- retrying against
 * a now-stale scheduled time would only compound the lateness), and the
 * failure additionally feeds uwb_cycle_ref_on_miss() so that a firmware that
 * is falling behind schedule re-syncs off a fresh SYNC frame on a later cycle
 * rather than continuing to schedule off a reference that may itself now be
 * stale.
 *
 * -----------------------------------------------------------------------
 * Out of scope (see UWB-252 ticket and sibling subissues)
 * -----------------------------------------------------------------------
 *   - The two-mode (responder <-> blinker) orchestration -- this module is
 *     the blinker standalone, mirroring UWB-232's responder-standalone scope.
 *   - Aloha join / slot assignment (UWB-11) -- self_addr / slot_idx /
 *     slot_count are plain injected configuration here, not derived.
 *   - Anchor-side reception or the TDoA solve.
 *   - Clock offset/drift disciplining beyond the cycle reference (ADR-003
 *     anchor-side arithmetic; out of scope for a tag, see uwb_cycle_ref.h).
 *
 * -----------------------------------------------------------------------
 * On-hardware bring-up (manual -- not automated by the ztest suite)
 * -----------------------------------------------------------------------
 * The ztest suite (tests/tracking/) exercises this module entirely against a
 * mocked deca_driver on the 'unit_testing' platform (same mock/stub pattern
 * as tests/dw1000_sync/); it cannot verify real RF timing, that a real
 * clock-master anchor's SYNC frame is correctly received, or that the blink
 * actually lands inside the assigned slot on-air. Before relying on this
 * module on a DWM1001 tag:
 *
 *   1. Two boards: a hand-built SYNC master (a second DWM1001 running a
 *      throwaway loop that periodically transmits a well-formed
 *      uwb_sync_frame_t via dw1000_tx_at() -- not part of this repo's
 *      committed samples, see UWB-242's bring-up note) and a tag running
 *      samples/tag_tracking.
 *   2. With the master OFF, confirm the tag logs UWB_TRACKING_NO_VALID_REF
 *      every cycle and never calls dw1000_tx_at() (observe via a logic
 *      analyser / RX-only LED on the DW1000, or simply the absence of any TX
 *      activity) -- confirms the mandatory gate holds on real hardware.
 *   3. Power on the master. Confirm the tag transitions to
 *      UWB_TRACKING_BLINKED within a few cycles of the first SYNC frame.
 *   4. Confirm the tag's blink lands inside its configured slot: capture
 *      both the master's SYNC TX and the tag's blink TX on a logic analyser
 *      / spectrum capture and confirm the time delta is approximately
 *      slot_idx * slot_duration_dtu (converted to seconds via the DW1000 tick
 *      rate, ~15.65 ps/tick) -- within the guard band folded into
 *      slot_duration_dtu (see uwb_slot_timing.h).
 *   5. Power off (or move out of range) the master again mid-run and confirm
 *      the tag goes back to UWB_TRACKING_NO_VALID_REF (not continuing to
 *      blink off the last-known reference) once CONFIG_UWB_SYNC_MAX_MISSED
 *      consecutive cycles have elapsed with no SYNC heard.
 *
 *   Record board IDs, firmware git SHA, and pass/fail per step in the PR or a
 *   bring-up log -- this module's PR is build-verified only, not
 *   hardware-verified, until this checklist has been run.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef UWB_TRACKING_H_
#define UWB_TRACKING_H_

#include <stdint.h>

#include "uwb_cycle_ref.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Injected per-tag / per-installation tracking configuration.
 *
 * All fields are provisioning-time values (UWB-11 Aloha join / slot
 * assignment provenance) -- this module treats them as opaque configuration,
 * not something it derives or validates against a live registration flow.
 */
typedef struct {
    uint16_t self_addr;         /**< This tag's registration-assigned 16-bit
                                      short address (embedded as the blink's
                                      MAC src_addr). */
    uint32_t slot_idx;           /**< This tag's registration-assigned TDMA
                                      slot index (0-based; see
                                      uwb_slot_timing.h). */
    uint32_t slot_count;          /**< Number of slots in the current
                                      superframe; bounds @c slot_idx. */
    uint32_t slot_duration_dtu;   /**< Per-slot duration, DW1000 time units,
                                      INCLUSIVE of the guard band (see
                                      uwb_slot_timing.h). */
} uwb_tracking_config_t;

/**
 * @brief Persistent per-tag tracking state, carried across cycles.
 *
 * Zero-initialise or call uwb_tracking_state_init() before the first call to
 * uwb_tracking_run_cycle().
 */
typedef struct {
    uwb_cycle_ref_t cycle_ref;  /**< Maintained cycle-start reference
                                      (UWB-243) -- the mandatory validity
                                      gate; see this header's top-of-file
                                      comment. */
    uint16_t blink_count;        /**< Per-tag 16-bit blink counter. Embedded
                                      in the blink payload and incremented
                                      ONLY when a blink is actually
                                      transmitted (dw1000_tx_at() succeeds) --
                                      never for a dropped/missed-window
                                      cycle. Wraps 0x0000-0xFFFF. */
    uint8_t  mac_seq;             /**< Per-tag MAC sequence number for blink
                                      frames. Incremented on every blink BUILD
                                      attempt (whether or not the subsequent
                                      TX actually succeeds), matching
                                      twr_responder.c's next_seq() convention
                                      -- the MAC sequence number is a framing
                                      detail, not a delivery-confirmation
                                      counter (that role belongs to
                                      @c blink_count above). Wraps 0x00-0xFF. */
} uwb_tracking_state_t;

/**
 * @brief Outcome of one uwb_tracking_run_cycle() call.
 */
typedef enum {
    /** A SYNC frame may or may not have been heard this cycle, but the cycle
     *  reference was valid and a blink was successfully built and
     *  transmitted in this tag's assigned slot. @c blink_count was
     *  incremented. */
    UWB_TRACKING_BLINKED = 0,

    /** The cycle reference is not currently valid (never synced, or sync
     *  lost per CONFIG_UWB_SYNC_MAX_MISSED, see uwb_cycle_ref.h) -- NO blink
     *  was attempted this cycle. This is the mandatory collision-avoidance
     *  gate; it is not an error, just "not safe to blink yet". */
    UWB_TRACKING_NO_VALID_REF,

    /** The cycle reference was valid and a blink was built, but
     *  dw1000_tx_at() failed to schedule/transmit it in time (the scheduled
     *  slot-TX window was already missed, e.g. -EIO/HPDWARN, or some other
     *  TX-schedule failure). The blink for this cycle is dropped -- NOT
     *  retried within this call -- and @c blink_count is NOT incremented.
     *  The cycle reference is also fed an uwb_cycle_ref_on_miss() so a
     *  firmware that is falling behind schedule re-syncs on a later cycle
     *  rather than compounding drift off a reference that may itself now be
     *  stale. */
    UWB_TRACKING_MISSED_WINDOW,

    /** The cycle reference was valid, but uwb_slot_tx_time() or
     *  uwb_build_tag_blink() rejected the (mis-)configured inputs (e.g.
     *  @c slot_idx >= @c slot_count) before any TX was attempted. Indicates a
     *  configuration bug, not a radio/timing condition -- dw1000_tx_at() is
     *  NOT called in this case. */
    UWB_TRACKING_BUILD_ERROR,
} uwb_tracking_outcome_t;

/**
 * @brief Initialise a tracking state to its "never synced, no blinks yet"
 *        starting point.
 *
 * @param state  State to initialise. No-op if NULL.
 */
void uwb_tracking_state_init(uwb_tracking_state_t *state);

/**
 * @brief Run exactly one superframe cycle of the tag tracking (blinker) loop.
 *
 * Sequence (see this header's top-of-file comment for the full rationale):
 *   1. dw1000_sync_rx(&info, sync_timeout_us). On success, feeds
 *      uwb_cycle_ref_on_sync(); on any failure (-ETIMEDOUT / -EIO /
 *      -EBADMSG), feeds uwb_cycle_ref_on_miss().
 *   2. uwb_cycle_ref_get(). If invalid, returns UWB_TRACKING_NO_VALID_REF
 *      WITHOUT scheduling a blink.
 *   3. uwb_slot_tx_time() to compute this cycle's scheduled blink TX time
 *      from the reference + @p cfg. On failure, returns
 *      UWB_TRACKING_BUILD_ERROR.
 *   4. uwb_build_tag_blink() using the CURRENT (pre-increment)
 *      @c state->blink_count. On failure, returns UWB_TRACKING_BUILD_ERROR.
 *   5. dw1000_tx_at() to schedule the blink. On -EIO (or any other
 *      scheduling failure), returns UWB_TRACKING_MISSED_WINDOW (see the enum
 *      doc for the on_miss() side-effect). On success, increments
 *      @c state->blink_count and returns UWB_TRACKING_BLINKED.
 *
 * Never blocks beyond the bounded @p sync_timeout_us RX wait -- this function
 * always returns, so callers drive the "listen forever" tracking loop with
 * their own `while (1)` calling this once per cycle (see
 * samples/tag_tracking/src/main.c).
 *
 * @param cfg               Per-tag / per-installation configuration
 *                            (self_addr, slot_idx, slot_count,
 *                            slot_duration_dtu). Must not be NULL.
 * @param state              Persistent tracking state, carried across calls.
 *                            Must not be NULL.
 * @param sync_timeout_us    Hardware RX timeout for the SYNC-frame listen,
 *                            forwarded to dw1000_sync_rx() /
 *                            dw1000_rx() (see dw1000_ranging.h for
 *                            units/semantics).
 *
 * @return  See uwb_tracking_outcome_t. NULL @p cfg or @p state returns
 *          UWB_TRACKING_BUILD_ERROR without touching the radio or @p state.
 */
uwb_tracking_outcome_t uwb_tracking_run_cycle(const uwb_tracking_config_t *cfg,
                                               uwb_tracking_state_t *state,
                                               uint32_t sync_timeout_us);

#ifdef __cplusplus
}
#endif

#endif /* UWB_TRACKING_H_ */
