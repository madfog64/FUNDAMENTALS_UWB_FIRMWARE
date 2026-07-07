/*! ----------------------------------------------------------------------------
 * @file    uwb_join.c
 * @brief   Tag-side Aloha join state machine (UWB-261). See uwb_join.h for
 *          the full API doc, state-flow rationale, and injected-seams
 *          contract.
 *
 * Pure composition -- every actual radio seam (SYNC RX, raw RX, delayed TX)
 * is implemented elsewhere and unit-tested in its own suite; this module
 * only sequences them per ADR-0039's tag-side state machine, plus the three
 * injected callbacks (eui64_get / rng / now_get) documented in uwb_join.h.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include "dw1000_ranging.h"
#include "dw1000_sync.h"
#include "uwb_frames.h"
#include "uwb_join.h"
#include "uwb_join_codec.h"

LOG_MODULE_REGISTER(uwb_join, LOG_LEVEL_DBG);

/*
 * RX scratch buffer for the AWAIT phase: sized larger than
 * UWB_SLOT_ASSIGNMENT_FRAME_SIZE so an oversized wrong frame is rejected by
 * uwb_parse_slot_assignment()'s own length check rather than dw1000_rx()'s
 * buffer-capacity guard (same rationale as dw1000_sync.c's
 * DW1000_SYNC_RX_BUF_SIZE).
 */
#define UWB_JOIN_AWAIT_RX_BUF_SIZE  64u

/*
 * dw1000_tx_at()'s 'len' parameter follows deca_driver convention: the TOTAL
 * frame length INCLUDING the 2-byte CRC the DW1000 hardware appends
 * automatically on TX (the buffer itself must NOT contain the CRC bytes) --
 * see dw1000_ranging.h's dw1000_tx_at() doc comment.
 */
#define UWB_JOIN_CRC_LEN  2u

/* A JOIN_REQUEST does not expect an automatic post-TX RX (unlike a DS-TWR
 * POLL) -- the tag arms its own listen for the SLOT_ASSIGNMENT separately
 * (the AWAIT phase's own dw1000_rx() call), so dw1000_tx_at()'s
 * expect_response is always false here. */
#define UWB_JOIN_TX_EXPECT_RESPONSE  false

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

/**
 * @brief Truncated-exponential backoff window (ADR-0039):
 *        window = min(backoff_base_cycles << attempt, backoff_max_cycles).
 *
 * 64-bit intermediate avoids the left-shift silently overflowing a uint32_t
 * for large @p attempt; an attempt so large the shift alone would exceed a
 * uint64_t (>= 64) clamps straight to backoff_max_cycles -- this cannot
 * actually happen in practice because cfg->max_attempts (if set) triggers a
 * give-up-and-restart long before @p attempt reaches such values, but the
 * guard keeps this function well-defined regardless of caller configuration.
 */
static uint32_t backoff_window_cycles(const uwb_join_config_t *cfg, uint32_t attempt)
{
    uint64_t window;

    if (attempt >= 64u) {
        return cfg->backoff_max_cycles;
    }

    window = (uint64_t)cfg->backoff_base_cycles << attempt;
    if (window > (uint64_t)cfg->backoff_max_cycles) {
        window = cfg->backoff_max_cycles;
    }

    return (uint32_t)window;
}

/**
 * @brief Reset to the machine's "cold start" phase: GATE if the gate is
 *        enabled, else straight to REQUEST. Used both by
 *        uwb_join_state_init() and by the give-up-and-restart path.
 */
static void restart_from_cold(const uwb_join_config_t *cfg, uwb_join_state_t *state)
{
    state->attempt = 0u;

    if (cfg->gate_cycles > 0u) {
        state->phase = UWB_JOIN_PHASE_GATE;
        state->cycles_remaining = cfg->gate_cycles;
    } else {
        state->phase = UWB_JOIN_PHASE_REQUEST;
        state->cycles_remaining = 0u;
    }
}

/**
 * @brief Enter backoff after a failed attempt (AWAIT window exhausted with
 *        no match, or a JOIN_REQUEST TX failure): draw a delay from the
 *        truncated-exponential window via cfg->rng(), increment the attempt
 *        counter, and either enter UWB_JOIN_PHASE_BACKOFF or -- if the
 *        attempt cap is hit -- give up and restart from cold (ADR-0039
 *        "give-up-and-restart policy").
 */
static void enter_backoff(const uwb_join_config_t *cfg, uwb_join_state_t *state)
{
    uint32_t window = backoff_window_cycles(cfg, state->attempt);
    uint32_t delay = cfg->rng(0u, window);

    state->attempt++;

    if (cfg->max_attempts > 0u && state->attempt >= cfg->max_attempts) {
        LOG_WRN("Join attempt cap (%u) reached -- giving up and restarting",
                (unsigned)cfg->max_attempts);
        restart_from_cold(cfg, state);
        return;
    }

    LOG_DBG("Entering backoff: attempt=%u window=%u delay=%u cycles",
            (unsigned)(state->attempt - 1u), (unsigned)window, (unsigned)delay);

    state->phase = UWB_JOIN_PHASE_BACKOFF;
    state->cycles_remaining = delay;
}

/**
 * @brief Shared "nothing usable happened this AWAIT cycle" bookkeeping:
 *        decrement the await window and, if exhausted, hand off to
 *        enter_backoff().
 *
 * @param foreign  true if a frame was heard but rejected (unparseable or
 *                  not addressed to us); false if nothing was heard at all
 *                  (RX timeout/error) -- only affects which outcome value
 *                  is returned to the caller, not the window bookkeeping.
 */
static uwb_join_step_outcome_t await_no_match(const uwb_join_config_t *cfg,
                                               uwb_join_state_t *state,
                                               bool foreign)
{
    if (state->cycles_remaining > 0u) {
        state->cycles_remaining--;
    }

    if (state->cycles_remaining == 0u) {
        LOG_DBG("Await window exhausted without a matching assignment");
        enter_backoff(cfg, state);
    }

    return foreign ? UWB_JOIN_STEP_AWAIT_FOREIGN : UWB_JOIN_STEP_AWAIT_TIMEOUT;
}

/* ---------------------------------------------------------------------------
 * Per-phase step handlers
 * --------------------------------------------------------------------------- */

static uwb_join_step_outcome_t step_gate(const uwb_join_config_t *cfg,
                                          uwb_join_state_t *state)
{
    uwb_sync_info_t info;
    int ret = dw1000_sync_rx(&info, cfg->sync_timeout_us);

    if (ret == 0) {
        LOG_DBG("Join gate: heard a SYNC -- proceeding to request");
        state->phase = UWB_JOIN_PHASE_REQUEST;
        return UWB_JOIN_STEP_GATE_SYNC_HEARD;
    }

    /* -ETIMEDOUT / -EIO / -EBADMSG are all "no SYNC this cycle" from the
     * gate's point of view -- see dw1000_sync_rx()'s doc comment. */
    if (state->cycles_remaining > 0u) {
        state->cycles_remaining--;
    }

    if (state->cycles_remaining == 0u) {
        LOG_DBG("Join gate window expired without a SYNC -- requesting anyway");
        state->phase = UWB_JOIN_PHASE_REQUEST;
    }

    return UWB_JOIN_STEP_GATE_LISTENING;
}

static uwb_join_step_outcome_t step_request(const uwb_join_config_t *cfg,
                                             uwb_join_state_t *state)
{
    uint8_t buf[UWB_JOIN_REQUEST_FRAME_SIZE];
    int build_len;
    uint64_t now_dtu;
    uint64_t tx_time_dtu;
    int ret;

    build_len = uwb_build_join_request(buf, state->eui64, state->mac_seq++,
                                        cfg->capabilities);
    if (build_len < 0) {
        LOG_ERR("uwb_build_join_request() failed (%d)", build_len);
        enter_backoff(cfg, state);
        return UWB_JOIN_STEP_REQUEST_TX_ERROR;
    }

    now_dtu = cfg->now_get();
    tx_time_dtu = dw1000_delayed_tx_time(now_dtu, cfg->tx_margin_dtu);

    ret = dw1000_tx_at(buf, (uint16_t)((uint32_t)build_len + UWB_JOIN_CRC_LEN),
                        tx_time_dtu, UWB_JOIN_TX_EXPECT_RESPONSE);
    if (ret != 0) {
        LOG_WRN("JOIN_REQUEST tx failed (%d) -- treating as a failed attempt", ret);
        enter_backoff(cfg, state);
        return UWB_JOIN_STEP_REQUEST_TX_ERROR;
    }

    LOG_DBG("JOIN_REQUEST transmitted (attempt %u)", (unsigned)state->attempt);

    state->phase = UWB_JOIN_PHASE_AWAIT;
    state->cycles_remaining = cfg->await_cycles;

    return UWB_JOIN_STEP_REQUEST_SENT;
}

static uwb_join_step_outcome_t step_await(const uwb_join_config_t *cfg,
                                           uwb_join_state_t *state)
{
    uint8_t buf[UWB_JOIN_AWAIT_RX_BUF_SIZE];
    uint16_t len = sizeof(buf);
    uint64_t rx_ts;
    int ret;

    ret = dw1000_rx(buf, &len, &rx_ts, cfg->assignment_timeout_us);
    if (ret == 0) {
        uwb_slot_assignment_t parsed;
        int parse_ret = uwb_parse_slot_assignment(buf, len, &parsed);

        if (parse_ret == 0 && uwb_join_assignment_is_for_me(state->eui64, &parsed)) {
            state->phase = UWB_JOIN_PHASE_JOINED;
            state->result.short_addr = parsed.short_addr;
            state->result.slot_idx = parsed.slot_idx;
            state->result.slot_count = parsed.slot_count;
            state->result.slot_duration_us = parsed.slot_duration_us;

            LOG_INF("Joined: short_addr=0x%04X slot=%u/%u duration=%uus",
                    parsed.short_addr, (unsigned)parsed.slot_idx,
                    (unsigned)parsed.slot_count, (unsigned)parsed.slot_duration_us);

            return UWB_JOIN_STEP_JOINED;
        }

        /* Either not a valid SLOT_ASSIGNMENT, or addressed to a different
         * tag -- ignore, no false adopt (ADR-0039). */
        return await_no_match(cfg, state, true);
    }

    /* -ETIMEDOUT / -EIO: nothing heard this cycle. */
    return await_no_match(cfg, state, false);
}

static uwb_join_step_outcome_t step_backoff(uwb_join_state_t *state)
{
    if (state->cycles_remaining > 0u) {
        state->cycles_remaining--;
    }

    if (state->cycles_remaining == 0u) {
        state->phase = UWB_JOIN_PHASE_REQUEST;
    }

    return UWB_JOIN_STEP_BACKOFF_WAITING;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

void uwb_join_state_init(uwb_join_state_t *state, const uwb_join_config_t *cfg)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));

    if (cfg == NULL || cfg->eui64_get == NULL) {
        /* Left zeroed (phase = UWB_JOIN_PHASE_GATE, cycles_remaining = 0).
         * Callers MUST call uwb_join_step() and check for
         * UWB_JOIN_STEP_INVALID_ARGS rather than relying on this state --
         * see uwb_join.h's uwb_join_state_init() doc comment. */
        return;
    }

    cfg->eui64_get(state->eui64);
    restart_from_cold(cfg, state);
}

uwb_join_step_outcome_t uwb_join_step(const uwb_join_config_t *cfg,
                                       uwb_join_state_t *state)
{
    if (cfg == NULL || state == NULL || cfg->eui64_get == NULL ||
        cfg->rng == NULL || cfg->now_get == NULL) {
        LOG_ERR("uwb_join_step(): NULL cfg/state or a required injected "
                "callback is NULL");
        return UWB_JOIN_STEP_INVALID_ARGS;
    }

    switch (state->phase) {
    case UWB_JOIN_PHASE_GATE:
        return step_gate(cfg, state);
    case UWB_JOIN_PHASE_REQUEST:
        return step_request(cfg, state);
    case UWB_JOIN_PHASE_AWAIT:
        return step_await(cfg, state);
    case UWB_JOIN_PHASE_BACKOFF:
        return step_backoff(state);
    case UWB_JOIN_PHASE_JOINED:
    default:
        return UWB_JOIN_STEP_ALREADY_JOINED;
    }
}

bool uwb_join_get_result(const uwb_join_state_t *state, uwb_join_result_t *out)
{
    if (state == NULL || state->phase != UWB_JOIN_PHASE_JOINED) {
        return false;
    }

    if (out != NULL) {
        *out = state->result;
    }

    return true;
}
