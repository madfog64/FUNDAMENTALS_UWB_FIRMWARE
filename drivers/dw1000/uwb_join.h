/*! ----------------------------------------------------------------------------
 * @file    uwb_join.h
 * @brief   Tag-side Aloha join state machine (UWB-261).
 *
 * ADR-0039 ("Tag join & slot-assignment protocol") specifies the behavioural
 * layer above the pinned contracts/uwb v0.6 JOIN_REQUEST (0x20) /
 * SLOT_ASSIGNMENT (0x21) frames (built/parsed by uwb_join_codec.c, UWB-259).
 * This module is the *tag-side* half of that ADR: request -> await
 * assignment -> truncated-exponential backoff/retry -> adopt config.
 * (The master-side registrar lives in the anchor repo, not here.)
 *
 * uwb_join_step() is a STEPPED function: one call advances the machine by
 * exactly one superframe-cycle's worth of work (one bounded listen, or one
 * TX, or one backoff tick) and always returns -- it never loops, blocks
 * beyond a bounded RX timeout, or owns a thread/timer of its own. Callers
 * (a future top-level tracking-mode composition, out of scope here -- see
 * UWB-263) drive the "join until JOINED" behaviour with their own
 * `while (1)` loop, exactly as uwb_tracking_run_cycle() (UWB-252) already
 * establishes for the per-cycle blinker.
 *
 * -----------------------------------------------------------------------
 * State flow (ADR-0039 "Tag-side join state machine")
 * -----------------------------------------------------------------------
 *   1. GATE (optional).  A bounded listen for >=1 SYNC frame
 *      (dw1000_sync_rx(), UWB-242) so the tag does not blind-TX a
 *      JOIN_REQUEST into a dead hall. The window is @c cfg->gate_cycles
 *      superframe cycles; it is skipped entirely (gate_cycles == 0) or
 *      satisfied early (a SYNC is heard); either way the machine proceeds to
 *      REQUEST once the window elapses, regardless of whether a SYNC was
 *      ever heard.
 *   2. REQUEST.  Build a JOIN_REQUEST (uwb_build_join_request(), UWB-259)
 *      with this tag's EUI-64 (from @c cfg->eui64_get -- see "Injected
 *      seams" below) and @c cfg->capabilities, then transmit it
 *      (src=0xFFFE, dest=0xFFFF, broadcast).
 *   3. AWAIT.  Listen for a SLOT_ASSIGNMENT whose target_eui64 matches this
 *      tag's own EUI-64 (uwb_parse_slot_assignment() +
 *      uwb_join_assignment_is_for_me(), UWB-259) for a window of
 *      @c cfg->await_cycles superframe cycles (ADR-0039: contract-suggested
 *      3-5). A SLOT_ASSIGNMENT that fails to parse, or parses but is
 *      addressed to a different tag, is ignored (does NOT reset the await
 *      window early) -- see UWB_JOIN_STEP_AWAIT_FOREIGN.
 *   4. BACKOFF + retry.  On an AWAIT window timing out with no match (or a
 *      JOIN_REQUEST TX failure), retry after a random delay drawn from a
 *      truncated-exponential window:
 *
 *          window = min(backoff_base_cycles << attempt, backoff_max_cycles)
 *          delay  = cfg->rng(0, window)
 *
 *      (attempt is the 0-based count of prior failed attempts since the
 *      last restart -- see "Injected seams" for cfg->rng). An attempt cap
 *      (@c cfg->max_attempts) triggers a full give-up-and-restart: the
 *      attempt counter resets to 0 and the machine re-enters GATE (or
 *      REQUEST directly if the gate is disabled) rather than continuing to
 *      grow the backoff window forever.
 *   5. ADOPT.  On a matching SLOT_ASSIGNMENT, the machine enters the
 *      terminal JOINED phase and exposes the adopted uwb_join_result_t
 *      (short_addr / slot_idx / slot_count / slot_duration_us) via
 *      uwb_join_get_result(). This module does NOT start blinking -- wiring
 *      the adopted config into the UWB-252 tracking loop is UWB-263's job,
 *      not this one's.
 *
 * -----------------------------------------------------------------------
 * Injected seams (why: deterministic host-testability, no radio-primitive
 * changes)
 * -----------------------------------------------------------------------
 * This module deliberately does NOT touch the deca_driver directly (unlike
 * dw1000_ranging.c) and does NOT gain any new radio primitive -- it composes
 * only the existing seams (dw1000_sync_rx(), dw1000_rx(), dw1000_tx_at(),
 * dw1000_delayed_tx_time(), the UWB-259 join codec) plus three injected
 * callbacks in uwb_join_config_t:
 *
 *   - eui64_get   Fills in this tag's real DW1000 EUI-64 (the register read
 *                  itself, e.g. via dwt_geteui(), is a bring-up/wiring
 *                  concern owned by the caller -- out of scope here, see
 *                  UWB-263). NEVER hard-coded.
 *   - rng          Draws the uniform backoff delay. Deterministic stubs make
 *                  the backoff/retry behaviour exactly reproducible in
 *                  tests/join/.
 *   - now_get      Returns the current 40-bit DW1000 system time (ticks).
 *                  dw1000_tx_at() (UWB-231) only supports a DELAYED TX --
 *                  there is no "transmit immediately" primitive, and adding
 *                  one is out of this ticket's scope (see UWB-231) -- so
 *                  scheduling the JOIN_REQUEST still goes through
 *                  dw1000_delayed_tx_time(cfg->now_get(), cfg->tx_margin_dtu)
 *                  exactly like every other delayed-TX caller in this
 *                  driver (see dw1000_ranging.h's own bring-up note
 *                  suggesting dwt_readsystimestamphi32() << 8 as the "now"
 *                  reference). Production wiring of this callback to the
 *                  real DW1000 system-time register is out of scope here
 *                  (no sample in this ticket).
 *
 * -----------------------------------------------------------------------
 * Out of scope (see UWB-261 ticket and sibling subissues)
 * -----------------------------------------------------------------------
 *   - Wiring the adopted uwb_join_result_t into the UWB-252 blinker, any
 *     runnable sample, and two-board bring-up -- UWB-263.
 *   - The master-side registrar (anchor repo) -- ADR-0039 "Master-side
 *     registrar" section.
 *   - Two-mode (join <-> tracking) orchestration.
 *   - Any contracts/uwb wire-format change (v0.6 stays pinned).
 *
 * -----------------------------------------------------------------------
 * On-hardware bring-up (manual -- not automated by the ztest suite)
 * -----------------------------------------------------------------------
 * The ztest suite (tests/join/) exercises this module entirely against a
 * mocked deca_driver (via the real dw1000_sync.c / dw1000_ranging.c) plus
 * stub eui64_get/rng/now_get callbacks on the 'unit_testing' platform; it
 * cannot verify real RF timing, that a real master-side registrar responds
 * correctly, or that the injected now_get()/rng() wiring is correct on real
 * hardware. Before relying on this module on a DWM1001 tag (requires UWB-263
 * to have wired real callbacks + a sample):
 *
 *   1. A real clock-master anchor (or stand-in, see dw1000_sync.h's
 *      bring-up note) running the ADR-0039 master-side registrar, plus a
 *      tag running the UWB-263 sample.
 *   2. With the master OFF (or its registrar not yet running), confirm the
 *      tag transmits a JOIN_REQUEST after the network-alive gate window
 *      elapses (observe on a logic analyser / spectrum capture) rather than
 *      hanging forever.
 *   3. Power on the master/registrar. Confirm the tag reaches JOINED within
 *      a few attempts and logs the adopted short_addr/slot_idx/slot_count/
 *      slot_duration_us.
 *   4. Power on two tags simultaneously (deliberate Aloha collision) and
 *      confirm both eventually join with DISTINCT short_addr/slot_idx pairs
 *      (the registrar's idempotent re-assignment + the tags' randomised
 *      backoff should de-correlate the retries).
 *   5. Confirm a tag's JOIN_REQUEST bears the correct EUI-64 (capture the
 *      frame and compare against the board's programmed EUI-64, e.g. via
 *      the vendor's production test tool) -- confirms the real eui64_get()
 *      wiring, not a stub/placeholder value.
 *
 *   Record board IDs, firmware git SHA, and pass/fail per step in the PR or
 *   a bring-up log -- this module's PR is build-verified only, not
 *   hardware-verified, until this checklist has been run (and until UWB-263
 *   exists to provide the sample/wiring in the first place).
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef UWB_JOIN_H_
#define UWB_JOIN_H_

#include <stdbool.h>
#include <stdint.h>

#include "uwb_join_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Injected getter: fill @p eui64_out with this tag's 64-bit DW1000
 *        EUI-64, little-endian byte order (LSB at index 0) -- the same
 *        order uwb_build_join_request() expects and
 *        uwb_join_assignment_is_for_me() compares against.
 *
 * This module never calls dwt_geteui() (deca_device_api.h) itself -- reading
 * the real register is a bring-up/wiring concern owned by the caller (see
 * this header's top-of-file "Injected seams" section).
 */
typedef void (*uwb_join_eui64_get_fn)(uint8_t eui64_out[UWB_EUI64_LEN]);

/**
 * @brief Injected RNG: draw a uniform random value in [@p min, @p max]
 *        (inclusive).
 *
 * Used only for the ADR-0039 truncated-exponential backoff delay draw
 * (delay = uniform(0, window)). Injected so the retry/backoff behaviour is
 * deterministically host-testable; production wiring supplies a real PRNG
 * elsewhere (out of scope here).
 *
 * @param min  Inclusive lower bound (always 0 for the current caller).
 * @param max  Inclusive upper bound (this attempt's backoff window, in
 *              superframe cycles).
 * @return     A value in [@p min, @p max].
 */
typedef uint32_t (*uwb_join_rng_fn)(uint32_t min, uint32_t max);

/**
 * @brief Injected "now": return the current 40-bit DW1000 system time
 *        (ticks), used as the reference timestamp for scheduling the
 *        JOIN_REQUEST's delayed TX (see this header's top-of-file "Injected
 *        seams" section for why a reference is needed at all).
 */
typedef uint64_t (*uwb_join_now_get_fn)(void);

/**
 * @brief Caller-injected join state-machine configuration.
 *
 * Every timing/count knob here is an ADR-0039 "empirical constant ...
 * provisioned/tunable, not baked into the wire" -- this module treats them
 * as opaque configuration, not something it derives or validates against a
 * live registration flow.
 */
typedef struct {
    uwb_join_eui64_get_fn eui64_get;  /**< Required. NULL rejected by
                                            uwb_join_step(). */
    uwb_join_rng_fn       rng;         /**< Required. NULL rejected by
                                            uwb_join_step(). */
    uwb_join_now_get_fn   now_get;     /**< Required. NULL rejected by
                                            uwb_join_step(). */

    uint8_t  capabilities;         /**< Capability bits for JOIN_REQUEST
                                         (e.g. UWB_JOIN_CAP_ROLE_REFERENCE
                                         iff this tag is a Phase-1
                                         calibration reference); reserved
                                         bits are masked to 0 by the codec. */

    uint32_t gate_cycles;          /**< Bounded network-alive gate window,
                                         superframe cycles. 0 disables the
                                         gate -- the machine starts straight
                                         at REQUEST. */
    uint32_t await_cycles;         /**< W: cycles to wait for a matching
                                         SLOT_ASSIGNMENT per attempt
                                         (ADR-0039 contract-suggested 3-5).
                                         Should be >= 1. */
    uint32_t backoff_base_cycles;  /**< W_base: backoff window at attempt 0
                                         (before any left-shift growth). */
    uint32_t backoff_max_cycles;   /**< W_max: backoff window growth cap. */
    uint32_t max_attempts;         /**< Give-up-and-restart cap on
                                         consecutive failed attempts
                                         (a JOIN_REQUEST sent with no
                                         matching assignment within the
                                         await window, or a JOIN_REQUEST TX
                                         failure). 0 = unlimited (never
                                         restarts). */

    uint32_t sync_timeout_us;        /**< RX timeout forwarded to
                                           dw1000_sync_rx() during the gate. */
    uint32_t assignment_timeout_us;  /**< RX timeout forwarded to
                                           dw1000_rx() while awaiting a
                                           SLOT_ASSIGNMENT. */
    uint32_t tx_margin_dtu;           /**< Margin (DW1000 time units) added
                                           to cfg->now_get()'s result when
                                           scheduling the JOIN_REQUEST's
                                           delayed TX -- see this header's
                                           top-of-file "Injected seams"
                                           section. */
} uwb_join_config_t;

/**
 * @brief Result adopted on a successful join (ADR-0039 "Adopt").
 */
typedef struct {
    uint16_t short_addr;        /**< Assigned 16-bit short address. */
    uint8_t  slot_idx;           /**< Assigned TDMA slot index. */
    uint8_t  slot_count;         /**< Total slots in the superframe. */
    uint16_t slot_duration_us;   /**< Slot duration, microseconds. */
} uwb_join_result_t;

/**
 * @brief Internal join-machine phase. Exposed for test/introspection;
 *        callers driving the machine should prefer uwb_join_get_result()
 *        and the uwb_join_step() return value over reading @c phase
 *        directly.
 */
typedef enum {
    UWB_JOIN_PHASE_GATE = 0,  /**< Bounded network-alive listen before the
                                    first request. */
    UWB_JOIN_PHASE_REQUEST,   /**< About to build + TX a JOIN_REQUEST. */
    UWB_JOIN_PHASE_AWAIT,     /**< Awaiting a matching SLOT_ASSIGNMENT. */
    UWB_JOIN_PHASE_BACKOFF,   /**< Counting down a random backoff delay. */
    UWB_JOIN_PHASE_JOINED,    /**< Terminal: assignment adopted. */
} uwb_join_phase_t;

/**
 * @brief Persistent join state, carried across uwb_join_step() calls.
 *
 * Must be initialised via uwb_join_state_init() before the first
 * uwb_join_step() call -- its initial contents are otherwise undefined.
 */
typedef struct {
    uwb_join_phase_t phase;
    uint8_t  eui64[UWB_EUI64_LEN];  /**< Cached at init from
                                          cfg->eui64_get(). */
    uint8_t  mac_seq;                /**< Per-tag MAC sequence number for
                                          JOIN_REQUEST frames; wraps
                                          0x00-0xFF. */
    uint32_t attempt;                 /**< 0-based attempt/backoff exponent
                                          index k; resets to 0 on a
                                          give-up-and-restart. */
    uint32_t cycles_remaining;        /**< Cycles left in the current phase
                                          (GATE / AWAIT / BACKOFF); unused
                                          (0) in REQUEST / JOINED. */
    uwb_join_result_t result;          /**< Populated once @c phase ==
                                          UWB_JOIN_PHASE_JOINED. */
} uwb_join_state_t;

/**
 * @brief Outcome of one uwb_join_step() call.
 */
typedef enum {
    UWB_JOIN_STEP_GATE_LISTENING,    /**< Gate active; no SYNC heard this
                                           call, window not yet exhausted
                                           (or exhausted THIS call -- either
                                           way the next call requests). */
    UWB_JOIN_STEP_GATE_SYNC_HEARD,   /**< Gate active; heard >=1 SYNC this
                                           call -- satisfied early, the next
                                           call requests. */
    UWB_JOIN_STEP_REQUEST_SENT,      /**< Built + transmitted a
                                           JOIN_REQUEST this call. */
    UWB_JOIN_STEP_REQUEST_TX_ERROR,  /**< Building or transmitting the
                                           JOIN_REQUEST failed this call --
                                           treated as a failed attempt (goes
                                           to backoff / give-up-and-restart,
                                           same as an AWAIT timeout). */
    UWB_JOIN_STEP_AWAIT_TIMEOUT,     /**< Awaiting; nothing heard this call,
                                           window not yet exhausted (or
                                           exhausted THIS call, entering
                                           backoff). */
    UWB_JOIN_STEP_AWAIT_FOREIGN,     /**< Awaiting; heard a frame that
                                           either fails to parse as a
                                           SLOT_ASSIGNMENT or parses but is
                                           not addressed to this tag's
                                           EUI-64 -- ignored (no false
                                           adopt), window not yet exhausted
                                           (or exhausted THIS call, entering
                                           backoff). */
    UWB_JOIN_STEP_BACKOFF_WAITING,   /**< Counting down the backoff delay --
                                           no radio activity this call. */
    UWB_JOIN_STEP_JOINED,            /**< Adopted a matching
                                           SLOT_ASSIGNMENT this call --
                                           terminal; see
                                           uwb_join_get_result(). */
    UWB_JOIN_STEP_ALREADY_JOINED,    /**< uwb_join_step() called again after
                                           JOINED -- no-op, no radio
                                           touched. */
    UWB_JOIN_STEP_INVALID_ARGS,      /**< NULL cfg/state, or a required
                                           injected callback (eui64_get /
                                           rng / now_get) is NULL -- no radio
                                           touched. */
} uwb_join_step_outcome_t;

/**
 * @brief Initialise a join state machine.
 *
 * Caches @p cfg->eui64_get()'s result into @c state->eui64, resets the
 * attempt/backoff counters, and enters UWB_JOIN_PHASE_GATE (if
 * @p cfg->gate_cycles > 0) or UWB_JOIN_PHASE_REQUEST (gate disabled).
 *
 * @param state  State to initialise. No-op if NULL.
 * @param cfg    Configuration to read eui64_get from. If NULL or
 *                 cfg->eui64_get is NULL, @p state is zeroed to a safe
 *                 (but not meaningfully usable) phase -- callers MUST call
 *                 uwb_join_step() and check for UWB_JOIN_STEP_INVALID_ARGS
 *                 rather than relying on @p state's contents in that case.
 */
void uwb_join_state_init(uwb_join_state_t *state, const uwb_join_config_t *cfg);

/**
 * @brief Advance the join state machine by exactly one superframe cycle /
 *        attempt step.
 *
 * Never blocks beyond the bounded RX timeouts in @p cfg
 * (sync_timeout_us / assignment_timeout_us) -- always returns, so callers
 * drive the "join until JOINED" loop by calling this once per superframe
 * cycle (mirroring uwb_tracking_run_cycle()'s (UWB-252) calling
 * convention). Performs AT MOST one radio operation per call (one
 * dw1000_sync_rx(), one dw1000_rx(), or one dw1000_tx_at()) -- see this
 * header's top-of-file state-flow section for which phase does which.
 *
 * @param cfg    Configuration. Must not be NULL; cfg->eui64_get, cfg->rng,
 *                 and cfg->now_get must not be NULL.
 * @param state  Persistent state, previously initialised by
 *                 uwb_join_state_init(). Must not be NULL.
 * @return  See uwb_join_step_outcome_t.
 */
uwb_join_step_outcome_t uwb_join_step(const uwb_join_config_t *cfg,
                                       uwb_join_state_t *state);

/**
 * @brief Read the adopted join result, if the machine has reached JOINED.
 *
 * @param[in]  state  State to read.
 * @param[out] out     Populated on success. May be NULL if the caller only
 *                       needs the boolean check.
 * @return  true   @p state->phase == UWB_JOIN_PHASE_JOINED; @p out (if
 *                  non-NULL) is populated.
 * @return  false  NULL @p state, or the machine has not joined yet. @p out
 *                  is left untouched.
 */
bool uwb_join_get_result(const uwb_join_state_t *state, uwb_join_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* UWB_JOIN_H_ */
