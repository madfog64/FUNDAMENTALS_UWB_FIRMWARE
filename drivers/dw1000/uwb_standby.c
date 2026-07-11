/*! ----------------------------------------------------------------------------
 * @file    uwb_standby.c
 * @brief   Tag-side STANDBY power-state machine (UWB-319). See uwb_standby.h
 *          for the full API doc, state-flow, and the on-hardware bring-up
 *          checklist.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <stddef.h>

#include <zephyr/logging/log.h>

#include "dw1000_sleep.h"
#include "dw1000_sync.h"
#include "uwb_standby.h"

LOG_MODULE_REGISTER(uwb_standby, LOG_LEVEL_DBG);

/* ---------------------------------------------------------------------------
 * enter_standby -- shared MONITORING -> ASLEEP transition.
 *
 * Calls the UWB-316 seam to deep-sleep the DW1000, arms the wake-cadence
 * deadline off cfg->now_get(), and switches state->phase to ASLEEP. Used by
 * both the "N missed cycles" and "no active session" triggers (see
 * uwb_standby.h's "State flow" section) -- both reach STANDBY the same way.
 * --------------------------------------------------------------------------- */
static void enter_standby(const uwb_standby_config_t *cfg, uwb_standby_state_t *state)
{
    (void)dw1000_sleep();

    state->phase = UWB_STANDBY_PHASE_ASLEEP;
    state->missed = 0;
    state->wake_deadline_ms = cfg->now_get() + (uint32_t)CONFIG_UWB_STANDBY_WAKE_CADENCE_MS;

    LOG_INF("STANDBY entered (DW1000 DEEPSLEEP); next presence-check wake in %u ms",
            (unsigned)CONFIG_UWB_STANDBY_WAKE_CADENCE_MS);
}

/* ---------------------------------------------------------------------------
 * re_sleep -- shared ASLEEP re-arm (no presence heard, or wake failed).
 *
 * Re-issues dw1000_sleep() and pushes the wake-cadence deadline out from the
 * current time (a fresh cfg->now_get() call, not the deadline that just
 * elapsed) so the cadence is measured from "when we finished this check",
 * not "when the last deadline was set" -- avoids the deadline creeping
 * earlier and earlier if a single step() call itself takes non-trivial wall
 * time (wake + listen can take tens of milliseconds, see dw1000_sleep.h).
 * --------------------------------------------------------------------------- */
static void re_sleep(const uwb_standby_config_t *cfg, uwb_standby_state_t *state)
{
    (void)dw1000_sleep();

    state->wake_deadline_ms = cfg->now_get() + (uint32_t)CONFIG_UWB_STANDBY_WAKE_CADENCE_MS;
}

/* ---------------------------------------------------------------------------
 * uwb_standby_state_init
 * --------------------------------------------------------------------------- */
void uwb_standby_state_init(uwb_standby_state_t *state)
{
    if (state == NULL) {
        return;
    }

    state->phase = UWB_STANDBY_PHASE_MONITORING;
    state->missed = 0;
    state->wake_deadline_ms = 0;
}

/* ---------------------------------------------------------------------------
 * uwb_standby_step
 * --------------------------------------------------------------------------- */
uwb_standby_step_outcome_t uwb_standby_step(const uwb_standby_config_t *cfg,
                                             uwb_standby_state_t *state,
                                             const uwb_standby_input_t *input)
{
    if (cfg == NULL || state == NULL || cfg->now_get == NULL) {
        return UWB_STANDBY_STEP_INVALID_ARGS;
    }

    /* -------------------------------------------------------------------
     * MONITORING -- watch for an entry trigger.
     * ------------------------------------------------------------------- */
    if (state->phase == UWB_STANDBY_PHASE_MONITORING) {
        if (input == NULL) {
            return UWB_STANDBY_STEP_INVALID_ARGS;
        }

        if (!input->session_active) {
            LOG_INF("STANDBY entry trigger: no active session");
            enter_standby(cfg, state);
            return UWB_STANDBY_STEP_ENTERED;
        }

        if (input->sync_ok) {
            state->missed = 0;
            return UWB_STANDBY_STEP_MONITORING;
        }

        state->missed++;
        if (state->missed > (uint32_t)CONFIG_UWB_SYNC_MAX_MISSED) {
            LOG_INF("STANDBY entry trigger: %u consecutive missed SYNC frames "
                    "(> CONFIG_UWB_SYNC_MAX_MISSED=%u)",
                    (unsigned)state->missed, (unsigned)CONFIG_UWB_SYNC_MAX_MISSED);
            enter_standby(cfg, state);
            return UWB_STANDBY_STEP_ENTERED;
        }

        return UWB_STANDBY_STEP_MONITORING;
    }

    /* -------------------------------------------------------------------
     * ASLEEP -- honour the wake cadence; @p input is not consulted.
     * ------------------------------------------------------------------- */
    {
        uint32_t now = cfg->now_get();

        /* Wrap-aware "is now >= deadline" check: cast the unsigned
         * difference to signed so a deadline that has rolled over relative
         * to now (or vice versa) still compares correctly -- the same idiom
         * as Zephyr's own sys_timepoint / k_uptime delta comparisons (see
         * uwb_standby_now_get_fn's doc comment for why a 32-bit ms wrap is
         * not otherwise a correctness concern here). */
        if ((int32_t)(now - state->wake_deadline_ms) < 0) {
            return UWB_STANDBY_STEP_ASLEEP_WAITING;
        }

        if (dw1000_wake() != DW1000_SLEEP_OK) {
            LOG_ERR("STANDBY wake failed (DW1000 unusable) -- re-sleeping "
                    "without a presence check");
            re_sleep(cfg, state);
            return UWB_STANDBY_STEP_WAKE_ERROR;
        }

        {
            uwb_sync_info_t info;
            int ret = dw1000_sync_rx(&info, cfg->sync_timeout_us);

            if (ret == 0) {
                LOG_INF("STANDBY: network presence detected (SYNC heard) -- "
                        "exiting STANDBY");
                state->phase = UWB_STANDBY_PHASE_MONITORING;
                state->missed = 0;
                return UWB_STANDBY_STEP_WOKE_PRESENT;
            }

            LOG_DBG("STANDBY: no presence this wake (dw1000_sync_rx=%d) -- "
                    "re-sleeping", ret);
            re_sleep(cfg, state);
            return UWB_STANDBY_STEP_WOKE_NO_PRESENCE;
        }
    }
}

/* ---------------------------------------------------------------------------
 * uwb_standby_is_asleep
 * --------------------------------------------------------------------------- */
bool uwb_standby_is_asleep(const uwb_standby_state_t *state)
{
    return (state != NULL) && (state->phase == UWB_STANDBY_PHASE_ASLEEP);
}
