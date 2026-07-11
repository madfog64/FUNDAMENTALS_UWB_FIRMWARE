/*! ----------------------------------------------------------------------------
 * @file    uwb_standby.h
 * @brief   Tag-side STANDBY power-state machine: DW1000 deep-sleep + periodic
 *          network-presence wake (UWB-319).
 *
 * Per ADR-0046 ("Tag power-management profile & low-power state model")
 * Decision point 3: "STANDBY is where the savings live." When there is no
 * active session (or SYNC has been lost past CONFIG_UWB_SYNC_MAX_MISSED), the
 * tag deep-sleeps the DW1000 (UWB-316's dw1000_sleep()/dw1000_wake() seam) and
 * wakes on a provisioned low-rate cadence to check for a live network (one
 * SYNC listen); on presence it exits STANDBY so the caller can (re)join
 * (ADR-0039).
 *
 * This module only *signals* the ACTIVE/STANDBY transition -- it does not run
 * the join flow (uwb_join.h, UWB-261) or the ACTIVE tracking loop
 * (uwb_tracking.h, UWB-252) itself; those are out of scope here (see
 * "Out of scope" below).
 *
 * -----------------------------------------------------------------------
 * Stepped state machine: uwb_standby_step()
 * -----------------------------------------------------------------------
 * uwb_standby_step() is a STEPPED function, exactly like uwb_join_step() and
 * uwb_tracking_run_cycle(): one call performs exactly one unit of work (one
 * missed/no-session check while MONITORING, or one wake-cadence check --
 * "one wake/check" -- while ASLEEP) and always returns. It never blocks
 * beyond the bounded RX timeout used by the presence-check SYNC listen, and
 * never loops or owns a thread/timer of its own. Callers drive it with their
 * own loop, calling it once per cycle while an active session (MONITORING) is
 * running, and once per housekeeping tick (whatever cadence that outer loop
 * runs at -- e.g. a slow k_timer -- see "Injected clock" below) while ASLEEP.
 *
 * -----------------------------------------------------------------------
 * State flow
 * -----------------------------------------------------------------------
 *   MONITORING (default / re-entered on presence, see WOKE_PRESENT below):
 *     Each uwb_standby_step() call is fed a uwb_standby_input_t describing
 *     what happened this cycle:
 *       - input->session_active == false ("no active session"): enters
 *         STANDBY immediately, regardless of the missed count -- calls
 *         dw1000_sleep() (UWB-316), transitions to ASLEEP, returns
 *         UWB_STANDBY_STEP_ENTERED.
 *       - input->session_active == true, input->sync_ok == true: a SYNC was
 *         heard this cycle -- resets the missed counter to 0, stays
 *         MONITORING, returns UWB_STANDBY_STEP_MONITORING.
 *       - input->session_active == true, input->sync_ok == false: a SYNC was
 *         missed this cycle -- increments the missed counter. Once it
 *         exceeds CONFIG_UWB_SYNC_MAX_MISSED (the SAME threshold
 *         uwb_cycle_ref_on_miss(), UWB-243, uses to invalidate the tag's
 *         cycle reference -- reused here, not duplicated, because "SYNC lost
 *         past CONFIG_UWB_SYNC_MAX_MISSED" is literally the ADR-0046 trigger
 *         condition), enters STANDBY the same way as the no-session case and
 *         returns UWB_STANDBY_STEP_ENTERED; otherwise stays MONITORING and
 *         returns UWB_STANDBY_STEP_MONITORING.
 *
 *   ASLEEP (entered from MONITORING via UWB_STANDBY_STEP_ENTERED):
 *     @p input is ignored entirely (there is no per-cycle sync outcome to
 *     report while the radio is deep-asleep). Each call:
 *       - Reads @p cfg->now_get() (see "Injected clock" below). If the
 *         elapsed time since the last sleep/re-sleep is still short of
 *         CONFIG_UWB_STANDBY_WAKE_CADENCE_MS, does nothing and returns
 *         UWB_STANDBY_STEP_ASLEEP_WAITING -- no radio touched.
 *       - Once the cadence deadline is reached: calls dw1000_wake()
 *         (UWB-316). If that fails (DW1000_SLEEP_ERR_WAKE_RECONFIGURE -- the
 *         radio did not come back), does NOT attempt a SYNC listen (the
 *         radio is not usable), makes a best-effort dw1000_sleep() call to
 *         return to a low-power state, resets the cadence deadline, stays
 *         ASLEEP, and returns UWB_STANDBY_STEP_WAKE_ERROR.
 *       - If the wake succeeds: listens for exactly one SYNC frame
 *         (dw1000_sync_rx(), UWB-242, with @p cfg->sync_timeout_us) as the
 *         network-presence check.
 *           - SYNC heard (return 0): network presence confirmed --
 *             transitions to MONITORING (missed counter reset to 0) and
 *             returns UWB_STANDBY_STEP_WOKE_PRESENT. The caller MUST now
 *             (re)join (uwb_join.h / uwb_join_track.h, out of scope here) --
 *             this module does NOT drive the join flow itself.
 *           - No SYNC heard (any other dw1000_sync_rx() return, e.g.
 *             -ETIMEDOUT): re-sleeps (dw1000_sleep()), resets the cadence
 *             deadline, stays ASLEEP, and returns
 *             UWB_STANDBY_STEP_WOKE_NO_PRESENCE.
 *
 * -----------------------------------------------------------------------
 * Injected clock (why: deterministic host-testability, no wall-clock
 * dependency in the pure logic)
 * -----------------------------------------------------------------------
 * Unlike the MONITORING phase (which is driven purely by per-cycle input
 * events, mirroring uwb_cycle_ref.h's on_sync()/on_miss() "passive state"
 * pattern -- no clock needed there), the ASLEEP phase's wake cadence is
 * measured in REAL elapsed time, not step() call counts: while the DW1000 is
 * deep-asleep there is no superframe cycle happening, so the outer loop
 * calling uwb_standby_step() while ASLEEP is under no obligation to run at
 * the tracking cadence -- it might be a slow housekeeping timer instead. This
 * module therefore takes an injected monotonic clock (@c cfg->now_get,
 * production wiring: k_uptime_get_32()) and measures the cadence deadline
 * against it, exactly the deterministic-injection rationale uwb_join.h's
 * cfg->now_get gives for its own (DW1000-tick, not wall-clock) "now"
 * callback.
 *
 * -----------------------------------------------------------------------
 * Kconfig-provisioned constants (ADR-0046 point 6: "empirical constants are
 * provisioned, not decided here")
 * -----------------------------------------------------------------------
 *   CONFIG_UWB_SYNC_MAX_MISSED           Reused from uwb_cycle_ref.h/UWB-243
 *                                         -- see the state-flow section above.
 *   CONFIG_UWB_STANDBY_WAKE_CADENCE_MS   New (this ticket) -- see
 *                                         drivers/dw1000/Kconfig for the
 *                                         documented default and rationale.
 * Both are read directly by uwb_standby.c (mirroring uwb_cycle_ref.c's own
 * direct CONFIG_UWB_SYNC_MAX_MISSED reference), not injected via
 * uwb_standby_config_t -- there is no per-caller reason either constant
 * would need to vary at runtime, only per-build (Kconfig) tuning on
 * hardware.
 *
 * -----------------------------------------------------------------------
 * Out of scope (see UWB-319 ticket and ADR-0046 "New work created")
 * -----------------------------------------------------------------------
 *   - The join flow itself (uwb_join.h/uwb_join_track.h, UWB-261/UWB-263,
 *     ADR-0039) -- this module only signals "network present, please
 *     (re)join" via UWB_STANDBY_STEP_WOKE_PRESENT; it does not drive
 *     uwb_join_step() itself.
 *   - The ACTIVE tracking loop (uwb_tracking.h, UWB-252) -- computing
 *     input->sync_ok each cycle (e.g. from uwb_cycle_ref_get()) is the
 *     caller's job, not this module's.
 *   - OFF / System-OFF storage state and battery monitoring (ADR-0046
 *     siblings UWB-322 and a future OFF-state ticket).
 *   - Real RF timing -- see "On-hardware bring-up" below.
 *
 * -----------------------------------------------------------------------
 * On-hardware bring-up (manual -- not automated by the ztest suite)
 * -----------------------------------------------------------------------
 * The ztest suite (tests/uwb_standby/) exercises this module entirely against
 * a mocked deca_driver (via the real dw1000_sleep.c / dw1000_sync.c /
 * dw1000_ranging.c / dw1000_config.c) plus a stub now_get() callback on the
 * 'unit_testing' platform; it cannot verify real RF timing, real current
 * draw, or that a real clock-master anchor is heard correctly after a real
 * wake. Before relying on this module on a DWM1001 tag:
 *
 *   1. Bring up dw1000_sleep.h's own bring-up checklist first (a prerequisite
 *      -- this module is built entirely on that seam).
 *   2. Drive uwb_standby_step() with input->session_active = false (or drive
 *      enough consecutive input->sync_ok = false cycles to exceed
 *      CONFIG_UWB_SYNC_MAX_MISSED) and confirm UWB_STANDBY_STEP_ENTERED is
 *      returned exactly once, coincident with dw1000_sleep() being called
 *      (log line / breakpoint). On a current probe / logic analyser, confirm
 *      DW1000 supply current drops to the DEEPSLEEP level within this call.
 *   3. Continue calling uwb_standby_step() (input ignored while ASLEEP) at
 *      whatever cadence the housekeeping loop uses. Confirm
 *      UWB_STANDBY_STEP_ASLEEP_WAITING is returned (no current change) until
 *      CONFIG_UWB_STANDBY_WAKE_CADENCE_MS of wall-clock time has elapsed
 *      (per cfg->now_get(), e.g. k_uptime_get_32()), then confirm the DW1000
 *      current returns to active levels for the duration of one SYNC listen
 *      before dropping back to DEEPSLEEP (UWB_STANDBY_STEP_WOKE_NO_PRESENCE)
 *      -- with no live SYNC master running yet, this is the expected
 *      steady-state cycle.
 *   4. Power on / bring into range a real clock-master anchor (or stand-in,
 *      see dw1000_sync.h's own bring-up note) broadcasting SYNC frames.
 *      Confirm the NEXT wake cycle returns UWB_STANDBY_STEP_WOKE_PRESENT and
 *      that the caller's integration (out of scope here, see "Out of scope")
 *      subsequently drives a successful join (uwb_join.h's own bring-up
 *      checklist).
 *   5. Confirm CONFIG_UWB_STANDBY_WAKE_CADENCE_MS's actual current-vs-latency
 *      tradeoff on real hardware (average current over a full
 *      sleep/wake/listen/re-sleep period) and re-tune the Kconfig default if
 *      the placeholder value does not meet the (not-yet-decided, ADR-0046
 *      "Open" point A) target field battery life.
 *
 *   Record board IDs, firmware git SHA, measured STANDBY current, and
 *   pass/fail per step in the PR or a bring-up log -- this module's PR is
 *   build-verified only, not hardware-verified, until this checklist has
 *   been run.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef UWB_STANDBY_H_
#define UWB_STANDBY_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Injected monotonic clock: return the current time in milliseconds.
 *
 * Production wiring: k_uptime_get_32(). Only ever used for elapsed-time
 * subtraction against the ASLEEP-phase wake-cadence deadline (see this
 * header's top-of-file "Injected clock" section) -- a 32-bit millisecond
 * wrap (~49.7 days of continuous uptime) is not a correctness concern at any
 * plausible CONFIG_UWB_STANDBY_WAKE_CADENCE_MS value, since the subtraction
 * is done in the same unsigned width and wraps correctly either way.
 *
 * Deterministic stubs make the wake-cadence timing exactly reproducible in
 * tests/uwb_standby/.
 */
typedef uint32_t (*uwb_standby_now_get_fn)(void);

/**
 * @brief Caller-injected STANDBY state-machine configuration.
 */
typedef struct {
    uwb_standby_now_get_fn now_get;  /**< Required. NULL rejected by
                                           uwb_standby_step(). */

    uint32_t sync_timeout_us;  /**< RX timeout forwarded to dw1000_sync_rx()
                                     for the ASLEEP-phase presence-check
                                     listen. */
} uwb_standby_config_t;

/**
 * @brief Which phase uwb_standby_step() is currently driving.
 */
typedef enum {
    UWB_STANDBY_PHASE_MONITORING = 0,  /**< Not in STANDBY; watching for an
                                             entry trigger each step(). */
    UWB_STANDBY_PHASE_ASLEEP,           /**< DW1000 DEEPSLEEP; waiting for the
                                             next wake-cadence deadline. */
} uwb_standby_phase_t;

/**
 * @brief Persistent STANDBY state, carried across uwb_standby_step() calls.
 *
 * Must be initialised via uwb_standby_state_init() before the first
 * uwb_standby_step() call.
 */
typedef struct {
    uwb_standby_phase_t phase;
    uint32_t missed;              /**< Consecutive input->sync_ok == false
                                        reports since the last input->sync_ok
                                        == true report or STANDBY entry, while
                                        MONITORING. Meaningless while ASLEEP. */
    uint32_t wake_deadline_ms;     /**< cfg->now_get() value at (or past)
                                        which the next ASLEEP presence-check
                                        wake is due. Meaningless while
                                        MONITORING. */
} uwb_standby_state_t;

/**
 * @brief Per-cycle input to uwb_standby_step(), reporting what the caller
 *        observed this cycle. Only consulted while
 *        @c state->phase == UWB_STANDBY_PHASE_MONITORING -- ignored (may be
 *        NULL) while ASLEEP.
 */
typedef struct {
    bool session_active;  /**< false: no active session at all (e.g. powered
                                but idle, or not yet joined) -- an immediate
                                STANDBY entry trigger regardless of the
                                missed count. true: an active session/join
                                attempt is in progress this cycle -- see
                                @c sync_ok. */
    bool sync_ok;           /**< Only consulted when @c session_active is
                                true: true if this cycle's SYNC was received
                                (resets the missed counter -- e.g. feed this
                                from uwb_cycle_ref_get()'s return value, or
                                the tracking loop's own per-cycle SYNC
                                result), false if it was missed (counts
                                toward CONFIG_UWB_SYNC_MAX_MISSED). */
} uwb_standby_input_t;

/**
 * @brief Outcome of one uwb_standby_step() call.
 */
typedef enum {
    /** MONITORING; no entry trigger this call -- no radio touched. */
    UWB_STANDBY_STEP_MONITORING,

    /** MONITORING -> ASLEEP transition this call (N misses exceeded, or
     *  input->session_active was false) -- dw1000_sleep() was called. */
    UWB_STANDBY_STEP_ENTERED,

    /** ASLEEP; the wake-cadence deadline has not yet been reached -- no
     *  radio touched. */
    UWB_STANDBY_STEP_ASLEEP_WAITING,

    /** ASLEEP; the wake-cadence deadline was reached this call, the DW1000
     *  woke and listened, but no SYNC was heard -- re-slept, stays ASLEEP. */
    UWB_STANDBY_STEP_WOKE_NO_PRESENCE,

    /** ASLEEP; the wake-cadence deadline was reached this call, the DW1000
     *  woke and heard a SYNC frame -- STANDBY exited, phase -> MONITORING
     *  (missed counter reset). The caller MUST now (re)join -- see this
     *  header's top-of-file "Out of scope" section. */
    UWB_STANDBY_STEP_WOKE_PRESENT,

    /** ASLEEP; the wake-cadence deadline was reached this call, but
     *  dw1000_wake() failed (DW1000_SLEEP_ERR_WAKE_RECONFIGURE) -- no SYNC
     *  listen was attempted, a best-effort dw1000_sleep() was issued, stays
     *  ASLEEP. */
    UWB_STANDBY_STEP_WAKE_ERROR,

    /** NULL @p cfg / @p state / @p cfg->now_get, or (while MONITORING) NULL
     *  @p input -- no radio touched. */
    UWB_STANDBY_STEP_INVALID_ARGS,
} uwb_standby_step_outcome_t;

/**
 * @brief Initialise a STANDBY state machine to MONITORING with a clean
 *        missed counter.
 *
 * @param state  State to initialise. No-op if NULL.
 */
void uwb_standby_state_init(uwb_standby_state_t *state);

/**
 * @brief Advance the STANDBY state machine by exactly one "wake/check" step.
 *
 * See this header's top-of-file "State flow" section for the full per-phase
 * behaviour. Never blocks beyond the bounded RX timeout used by the
 * ASLEEP-phase presence-check listen (@c cfg->sync_timeout_us) -- always
 * returns.
 *
 * @param cfg     Configuration. Must not be NULL; @c cfg->now_get must not
 *                  be NULL.
 * @param state   Persistent state, previously initialised by
 *                  uwb_standby_state_init(). Must not be NULL.
 * @param input   Per-cycle input. Consulted only while
 *                  @c state->phase == UWB_STANDBY_PHASE_MONITORING (must not
 *                  be NULL in that case); ignored (may be NULL) while
 *                  @c state->phase == UWB_STANDBY_PHASE_ASLEEP.
 * @return  See uwb_standby_step_outcome_t.
 */
uwb_standby_step_outcome_t uwb_standby_step(const uwb_standby_config_t *cfg,
                                             uwb_standby_state_t *state,
                                             const uwb_standby_input_t *input);

/**
 * @brief Convenience predicate: is the STANDBY machine currently ASLEEP
 *        (DW1000 in DEEPSLEEP)?
 *
 * @param state  State to read. NULL is treated as "not asleep".
 * @return  true iff @c state->phase == UWB_STANDBY_PHASE_ASLEEP.
 */
bool uwb_standby_is_asleep(const uwb_standby_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* UWB_STANDBY_H_ */
