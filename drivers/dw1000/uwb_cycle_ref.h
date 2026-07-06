/*! ----------------------------------------------------------------------------
 * @file    uwb_cycle_ref.h
 * @brief   Tag-side maintained cycle reference from a stream of parsed SYNC
 *          frames (UWB-243).
 *
 * UWB-242 (dw1000_sync.c) gives the tag a validated, parsed SYNC frame per
 * cycle (uwb_sync_info_t: rx_ts, cycle_seq, master_tx_ts). This module turns
 * that single-frame result into a small, maintained state machine: the most
 * recent valid cycle-start reference, tolerant of an occasional dropped SYNC,
 * and invalidated when sync is lost for too long so the tag never schedules
 * a blink off a stale epoch.
 *
 * -----------------------------------------------------------------------
 * Scope: coarse cycle alignment for TDMA, NOT clock sync
 * -----------------------------------------------------------------------
 * TDoA (ADR-001 §Phase 2) cancels the tag's unknown TX time -- tag slot
 * timing only needs to avoid colliding with other tags' slots; positioning
 * precision comes entirely from the anchors' RX timestamps. This module
 * therefore keys the reference ONLY off the tag's own local sync_rx_ts (the
 * hardware RX timestamp of the SYNC frame, already in the tag's own clock
 * domain -- see uwb_sync_info_t::rx_ts).
 *
 * uwb_sync_info_t::master_tx_ts is carried through unchanged by
 * uwb_cycle_ref_on_sync() for a possible future consumer but is NOT read,
 * interpreted, or used by any function here. In particular, this module does
 * NOT do the ADR-003 clock offset/drift estimation
 * (T_rx_slave - (master_tx_ts + T_prop)) -- that is anchor-side arithmetic
 * (anchor repo) and is out of scope for a tag. Do not add it here.
 *
 * -----------------------------------------------------------------------
 * The UWB-9 seam: uwb_cycle_ref_get()
 * -----------------------------------------------------------------------
 * UWB-9 (the tracking-mode loop / blink slot scheduler) is the sole intended
 * consumer of uwb_cycle_ref_get(). The per-cycle top-level loop that calls
 * uwb_cycle_ref_on_sync() / uwb_cycle_ref_on_miss() also lives in UWB-9's
 * tracking-mode state machine, not here -- this module is passive state, not
 * a scheduler.
 *
 * UWB-9 MUST check the returned bool before scheduling a blink TX. When it
 * is false (never synced, or sync lost -- see uwb_cycle_ref_on_miss()), the
 * tag MUST NOT transmit a blink: there is no trustworthy cycle-start epoch
 * to schedule against, and blinking anyway risks colliding with another
 * tag's slot in a superframe the tag can no longer see the true start of.
 *
 * When the returned bool is true, UWB-9 computes its scheduled blink TX time
 * as:
 *
 *     slot_tx_time = sync_rx_ts + slot_idx * slot_duration_dtu
 *
 * where sync_rx_ts is this cycle's reference (the *out parameter, the tag's
 * local RX edge for the most recent valid SYNC), slot_idx is this tag's
 * registration-assigned TDMA slot index, and slot_duration_dtu is the
 * per-slot duration in DW1000 time units (ADR-001 superframe layout). Both
 * slot_idx and slot_duration_dtu are UWB-9 concerns, not this module's.
 *
 * -----------------------------------------------------------------------
 * On-hardware bring-up
 * -----------------------------------------------------------------------
 * None needed. This module is pure logic over a plain struct -- no
 * deca_driver, no radio, no timers -- and is exercised entirely by
 * tests/uwb_cycle_ref/ on the 'unit_testing' platform. It is build-verified
 * only (native_sim and nrf52dk_nrf52832 compile it as part of the drivers/
 * dw1000 library); there is nothing to observe on real hardware that the
 * unit tests do not already cover.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef UWB_CYCLE_REF_H_
#define UWB_CYCLE_REF_H_

#include <stdbool.h>
#include <stdint.h>

#include "dw1000_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tag-side maintained cycle reference.
 *
 * Zero-initialise or call uwb_cycle_ref_init() before first use.
 */
typedef struct {
    uint64_t sync_rx_ts;  /**< Tag's local RX timestamp of the most recent
                                valid SYNC frame (uwb_sync_info_t::rx_ts).
                                Only meaningful when @c valid is true. */
    uint16_t cycle_seq;   /**< Cycle sequence number of the most recent valid
                                SYNC frame. Only meaningful when @c valid is
                                true. */
    bool valid;           /**< True if the reference is current and safe for
                                UWB-9 to schedule a blink against. */
    uint32_t missed;      /**< Consecutive missed-SYNC count since the last
                                accepted uwb_cycle_ref_on_sync(). Reset to 0
                                by uwb_cycle_ref_on_sync(); incremented by
                                uwb_cycle_ref_on_miss(). */
} uwb_cycle_ref_t;

/**
 * @brief Initialise a cycle reference to its "never synced" state.
 *
 * Sets @c valid = false and @c missed = 0. @c sync_rx_ts / @c cycle_seq are
 * left undefined (not meaningful until the first uwb_cycle_ref_on_sync()) --
 * callers must not read them before @c valid is true.
 *
 * @param r  Reference to initialise. No-op if NULL.
 */
void uwb_cycle_ref_init(uwb_cycle_ref_t *r);

/**
 * @brief Feed one newly-received, already-validated SYNC frame into the
 *        reference.
 *
 * Re-anchors the reference to @p s : sets @c sync_rx_ts / @c cycle_seq from
 * @p s, sets @c valid = true, and resets @c missed = 0 -- unconditionally,
 * including when the reference was previously invalid (this is how a tag
 * re-acquires after a sync loss, see uwb_cycle_ref_on_miss()).
 *
 * Discontinuity detection: if the reference was already valid, the expected
 * next cycle_seq is (previous cycle_seq + 1) mod 2^16 (uint16_t wraparound
 * arithmetic). If @p s->cycle_seq does not match, a warning is logged --
 * the master may have skipped a cycle, or the tag may have missed more SYNC
 * frames than uwb_cycle_ref_on_miss() was told about -- but @p s is still
 * accepted and the reference still re-anchors to it. A jump is never treated
 * as a validation failure: uwb_sync_parse() (UWB-242) already guarantees
 * @p s is a well-formed SYNC frame; this module's job is only to track the
 * cycle-start epoch, not to police the master's cycle numbering.
 *
 * No discontinuity check is performed when the reference was NOT already
 * valid (first sync ever, or first sync after a sync loss) -- there is no
 * meaningful "previous cycle_seq" to compare against in that case.
 *
 * @param r  Reference to update. No-op if NULL.
 * @param s  Newly received, validated SYNC info (see dw1000_sync.h). No-op
 *           if NULL.
 */
void uwb_cycle_ref_on_sync(uwb_cycle_ref_t *r, const uwb_sync_info_t *s);

/**
 * @brief Record that the tag listened for a SYNC frame this cycle and did
 *        not get one (dw1000_sync_rx() returned a non-zero passthrough
 *        error -- see dw1000_sync.h).
 *
 * Increments @c missed. When @c missed exceeds CONFIG_UWB_SYNC_MAX_MISSED,
 * the reference is invalidated (@c valid = false) -- the tag has lost sync
 * for too long to trust the last-known epoch, and MUST re-acquire (via a
 * fresh uwb_cycle_ref_on_sync()) before scheduling another blink.
 *
 * @param r  Reference to update. No-op if NULL.
 */
void uwb_cycle_ref_on_miss(uwb_cycle_ref_t *r);

/**
 * @brief Read the current cycle reference, if valid.
 *
 * This is the seam UWB-9 (tracking-mode blink scheduler) reads. UWB-9 MUST
 * check the returned bool before scheduling a blink TX -- see the "The
 * UWB-9 seam" section in this header's top-of-file comment for the
 * slot_tx_time formula the returned @p sync_rx_ts feeds into.
 *
 * @param[in]  r             Reference to read.
 * @param[out] sync_rx_ts    Set to @p r->sync_rx_ts when the return value is
 *                            true. May be NULL if the caller only needs the
 *                            validity check. Untouched when the return value
 *                            is false.
 * @param[out] cycle_seq     Set to @p r->cycle_seq when the return value is
 *                            true. May be NULL. Untouched when the return
 *                            value is false.
 *
 * @return  true   The reference is valid; outputs are populated.
 * @return  false  @p r is NULL, or the reference has never been synced, or
 *                  sync has been lost (see uwb_cycle_ref_on_miss()). The
 *                  caller MUST NOT schedule a blink against stale/undefined
 *                  outputs in this case.
 */
bool uwb_cycle_ref_get(const uwb_cycle_ref_t *r, uint64_t *sync_rx_ts,
                       uint16_t *cycle_seq);

#ifdef __cplusplus
}
#endif

#endif /* UWB_CYCLE_REF_H_ */
