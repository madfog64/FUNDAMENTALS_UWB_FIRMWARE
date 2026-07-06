/*! ----------------------------------------------------------------------------
 * @file    twr_responder.h
 * @brief   DS-TWR responder mode state machine (UWB-232).
 *
 * During Phase-1 calibration (ADR-001 §Phase 1) a tag acts as a DS-TWR
 * *responder*: an anchor (or another reference device) initiates the
 * exchange with a POLL addressed to this device's short address; the
 * responder answers with a scheduled RESPONSE carrying its own T2/T3
 * timestamps, then waits for the initiator's FINAL to recover T1/T4/T5 and
 * capture its own T6. See drivers/dw1000/uwb_frames.h §"DS-TWR calibration
 * frames" and contracts/uwb/README.md §"DS-TWR calibration frames" for the
 * full six-timestamp exchange and range formula.
 *
 * This module wires together:
 *   - uwb_twr_codec.c  (UWB-230) — frame build/parse (pure byte manipulation)
 *   - dw1000_ranging.c (UWB-231) — dw1000_rx() / dw1000_tx_at() /
 *                                  dw1000_delayed_tx_time() radio primitives
 *
 * twr_responder_run_once() performs exactly one attempt: arm RX for a POLL,
 * and if one arrives addressed to @p self_addr, complete the RESPONSE/FINAL
 * exchange. It always returns (bounded by the RX timeouts below) rather than
 * blocking indefinitely, so callers drive the "listen forever" behaviour with
 * their own loop (see samples/twr_responder/src/main.c) — this keeps the
 * state machine itself synchronous and directly unit-testable.
 *
 * Out of scope (see UWB-232 ticket and sibling subissues):
 *   - Two-mode (responder <-> blinker) switch + registration/join (UWB-9/10/11).
 *   - Reporting T1..T6 to the cloud — the tag is UWB-only (ADR-011); this
 *     module captures the six timestamps locally and does NOT transmit them
 *     anywhere. How an anchor<->reference-tag range reaches the cloud is a
 *     cross-repo anchor/contracts concern.
 *   - Interrupt-driven RX (dw1000_rx() is polled, per UWB-231).
 *
 * -----------------------------------------------------------------------
 * On-hardware bring-up (manual — not automated by the ztest suite)
 * -----------------------------------------------------------------------
 * The ztest suite (tests/twr_responder/) exercises this module entirely
 * against the mocked deca_driver (same mock as tests/dw1000_ranging/) on the
 * 'unit_testing' platform; it cannot verify real RF timing or a real DS-TWR
 * exchange against another board. Before relying on this module on a
 * DWM1001 tag:
 *
 *   1. Two DWM1001 boards, both PHY-configured via dw1000_configure().
 *   2. Board A ("tag"): flash samples/twr_responder, call
 *      twr_responder_run_once(self_addr) (or the sample's loop) with its own
 *      assigned short address.
 *   3. Board B ("anchor" / DS-TWR initiator): run a DS-TWR initiator that
 *      transmits a POLL addressed to Board A's short address, then RESPONSE
 *      RX, then a scheduled FINAL — either a full anchor-host DS-TWR
 *      initiator (out of scope here, lives in the anchor repo) or a second
 *      DWM1001 running a throwaway initiator sketch built on the same
 *      dw1000_ranging.c primitives.
 *   4. Confirm Board A logs a completed exchange (TWR_RESPONDER_EXCHANGE_OK)
 *      with a plausible initiator_addr / exchange_id.
 *   5. With CONFIG_UWB_TWR_RESPONDER_DEBUG_RANGE=y, confirm the logged range
 *      is plausible for the known physical separation between the two
 *      boards (within roughly a metre before antenna-delay calibration,
 *      per ADR-001 — antenna delay is NOT calibrated by this module).
 *   6. Move Board A out of range (or power it off) and confirm
 *      twr_responder_run_once() returns TWR_RESPONDER_NO_POLL rather than
 *      hanging.
 *   7. Have Board B stop before sending FINAL (e.g. kill the initiator after
 *      RESPONSE RX) and confirm Board A's run returns TWR_RESPONDER_NO_FINAL.
 *
 *   Record board IDs, firmware git SHA, and pass/fail per step in the PR or a
 *   bring-up log — this module's PR is build-verified only, not
 *   hardware-verified, until this checklist has been run.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef TWR_RESPONDER_H_
#define TWR_RESPONDER_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Outcome of one twr_responder_run_once() attempt.
 *
 * Every non-OK value means "no exchange completed this attempt; the caller's
 * next call re-arms from scratch" — none of them represent a fatal/permanent
 * error. Distinguishing them is useful for logging/diagnostics only.
 */
typedef enum {
    /** Full DS-TWR exchange completed: RESPONSE sent, matching FINAL
     *  received. @p out (if non-NULL) is fully populated. */
    TWR_RESPONDER_EXCHANGE_OK = 0,

    /** No POLL was heard before the RX timeout
     *  (CONFIG_UWB_TWR_RESPONDER_POLL_TIMEOUT_US) expired, or an RX error was
     *  latched while waiting. Idle; re-armed on the next call. */
    TWR_RESPONDER_NO_POLL,

    /** A frame was heard while waiting for a POLL, but it is not a valid POLL
     *  addressed to this responder (wrong frame type / PAN ID / malformed, or
     *  a well-formed POLL addressed to a different short address). Ignored;
     *  re-armed on the next call. */
    TWR_RESPONDER_FOREIGN_FRAME,

    /** A valid POLL was received but dw1000_tx_at() failed to schedule/send
     *  the RESPONSE (e.g. a missed slot / HPDWARN). Exchange aborted; re-armed
     *  on the next call. */
    TWR_RESPONDER_TX_ERROR,

    /** The RESPONSE was sent but no valid FINAL was heard before the RX
     *  timeout (CONFIG_UWB_TWR_RESPONDER_FINAL_TIMEOUT_US) expired, or an RX
     *  error was latched while waiting. Exchange aborted; re-armed on the
     *  next call. */
    TWR_RESPONDER_NO_FINAL,

    /** A well-formed FINAL was heard while waiting, but its exchange_id does
     *  not match the exchange this responder is currently completing (e.g. a
     *  stale/foreign FINAL). Ignored; exchange aborted; re-armed on the next
     *  call. */
    TWR_RESPONDER_FINAL_MISMATCH,
} twr_responder_status_t;

/**
 * @brief Result of a completed DS-TWR exchange (TWR_RESPONDER_EXCHANGE_OK).
 *
 * Timestamp naming follows the T1..T6 convention in uwb_frames.h §"DS-TWR
 * calibration frames": T1/T4/T5 come from the initiator's FINAL payload,
 * T2/T3 are this responder's own (also echoed in the RESPONSE it sent), and
 * T6 is this responder's local RX timestamp of the FINAL.
 *
 * Per ADR-011 / this ticket's scope, these timestamps are captured for local
 * use (e.g. the optional debug range below) only — this module does not
 * report them anywhere.
 */
typedef struct {
    uint16_t initiator_addr; /**< Initiator's 16-bit short address (POLL src_addr). */
    uint16_t exchange_id;    /**< Exchange correlation id (from the POLL). */

    uint64_t poll_tx_ts;     /**< T1 — initiator's POLL TX timestamp (from FINAL). */
    uint64_t poll_rx_ts;     /**< T2 — this responder's POLL RX timestamp. */
    uint64_t resp_tx_ts;     /**< T3 — this responder's scheduled RESPONSE TX timestamp. */
    uint64_t resp_rx_ts;     /**< T4 — initiator's RESPONSE RX timestamp (from FINAL). */
    uint64_t final_tx_ts;    /**< T5 — initiator's scheduled FINAL TX timestamp (from FINAL). */
    uint64_t final_rx_ts;    /**< T6 — this responder's FINAL RX timestamp. */

    /** Local DS-TWR range estimate, in millimetres, computed from T1..T6 via
     *  uwb_twr_range_mm(). Only populated when
     *  CONFIG_UWB_TWR_RESPONDER_DEBUG_RANGE=y; otherwise left at 0.
     *  Debug/diagnostic aid only — antenna delay is NOT applied here beyond
     *  whatever dw1000_configure() already programmed into hardware, and this
     *  value is never reported off-device (ADR-011). */
    int64_t  range_mm;
} uwb_twr_exchange_t;

/**
 * @brief Run one DS-TWR responder attempt.
 *
 * Sequence:
 *   1. dw1000_rx() for a POLL (CONFIG_UWB_TWR_RESPONDER_POLL_TIMEOUT_US).
 *      Parses it with uwb_parse_twr_poll() and checks the frame's
 *      destination address against @p self_addr (uwb_parse_twr_poll() does
 *      not itself surface dest_addr).
 *   2. T2 = the POLL's RX timestamp; T3 = dw1000_delayed_tx_time(T2,
 *      CONFIG_UWB_TWR_RESP_REPLY_DELAY_DTU).
 *   3. Builds a RESPONSE (uwb_build_twr_response(), echoing the POLL's
 *      exchange_id and embedding T2/T3) and schedules it with dw1000_tx_at()
 *      (expect_response = true, so the receiver auto-arms after TX).
 *   4. dw1000_rx() for a FINAL (CONFIG_UWB_TWR_RESPONDER_FINAL_TIMEOUT_US).
 *      Parses it with uwb_parse_twr_final() and checks the exchange_id
 *      matches step 1's POLL.
 *   5. On success, captures T6 (this frame's RX timestamp) plus T1/T4/T5 from
 *      the FINAL payload.
 *
 * Never blocks indefinitely — every RX wait is bounded by the corresponding
 * Kconfig timeout, so this function always returns.
 *
 * @param self_addr   This device's 16-bit short registered address; POLLs
 *                     addressed to any other value are treated as foreign
 *                     (TWR_RESPONDER_FOREIGN_FRAME).
 * @param[out] out     Populated on TWR_RESPONDER_EXCHANGE_OK. May be NULL if
 *                     the caller only cares about the status code. Left
 *                     untouched (zeroed) on any non-OK return.
 *
 * @return  See twr_responder_status_t.
 */
twr_responder_status_t twr_responder_run_once(uint16_t self_addr,
                                               uwb_twr_exchange_t *out);

/**
 * @brief Pure DS-TWR range computation from the six exchange timestamps.
 *
 * Implements the range formula in drivers/dw1000/uwb_frames.h §"DS-TWR
 * calibration frames" / contracts/uwb/README.md §"DS-TWR calibration
 * frames":
 *
 *   T_round1 = T4 - T1,  T_reply1 = T3 - T2
 *   T_round2 = T6 - T3,  T_reply2 = T5 - T4
 *   ToF      = (T_round1*T_round2 - T_reply1*T_reply2)
 *              / (T_round1 + T_round2 + T_reply1 + T_reply2)
 *   distance [mm] = ToF * 4.691763   (1 DW1000 tick ~= 4.691763 mm one-way)
 *
 * All timestamp differences are taken modulo 2^40 (the DW1000 system time
 * counter width), matching every other timestamp difference in this
 * codebase (e.g. dw1000_ranging.c's dw1000_delayed_tx_time()).
 *
 * This is a pure function — no deca_driver / hardware dependency — so it is
 * directly unit-testable regardless of CONFIG_UWB_TWR_RESPONDER_DEBUG_RANGE.
 * twr_responder_run_once() only *calls* it (and logs the result) when that
 * Kconfig option is enabled; see uwb_twr_exchange_t::range_mm.
 *
 * Debug/diagnostic aid only: no antenna-delay recalibration, no NLOS/outlier
 * rejection, and never reported off-device (ADR-011).
 *
 * @return  Range estimate in millimetres. 0 if the four timestamp deltas sum
 *          to zero (degenerate input).
 */
int64_t uwb_twr_range_mm(uint64_t t1, uint64_t t2, uint64_t t3,
                          uint64_t t4, uint64_t t5, uint64_t t6);

#ifdef __cplusplus
}
#endif

#endif /* TWR_RESPONDER_H_ */
