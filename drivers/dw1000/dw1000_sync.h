/*! ----------------------------------------------------------------------------
 * @file    dw1000_sync.h
 * @brief   Tag-side SYNC-frame RX + validation/parse over dw1000_rx (UWB-242).
 *
 * In TDoA tracking (ADR-001 §Phase 2) the clock-master anchor broadcasts a
 * SYNC / cycle-start frame (UWB_FRAME_TYPE_SYNC, contracts/uwb_frames.h) at
 * the start of every superframe cycle. This module lets a *tag* receive that
 * frame, validate it against the wire format, and extract the two per-cycle
 * fields it carries (cycle_seq, master_tx_ts) plus the tag's own hardware RX
 * timestamp of the frame.
 *
 * For the tag, SYNC RX is COARSE superframe/cycle alignment for TDMA (so the
 * tag's blink lands in its assigned slot without colliding with other tags,
 * sibling UWB-243) -- NOT sub-nanosecond clock sync. The ADR-003 offset/drift
 * math (T_rx_slave - (master_tx_ts + T_prop)) is anchor-side arithmetic and
 * lives in the anchor repo; this module only gets the tag as far as "I heard
 * cycle N start at my local time T, and the master said it transmitted at
 * master_tx_ts".
 *
 * This module reuses dw1000_ranging.c's dw1000_rx() for the actual radio
 * RX + hardware timestamp -- it does not touch the deca_driver directly.
 *
 * Out of scope (see sibling UWB-243 and UWB-9):
 *   - Maintaining a tag-side cycle reference (epoch tracking, missed-sync /
 *     resync handling).
 *   - The "listen once per cycle" tracking-mode loop / state machine.
 *   - Blink TX + slot scheduling.
 *   - Anchor-side ADR-003 clock offset/drift math.
 *
 * -----------------------------------------------------------------------
 * On-hardware bring-up (manual -- not automated by the ztest suite)
 * -----------------------------------------------------------------------
 * The ztest suite (tests/dw1000_sync/) exercises this module entirely against
 * a mocked deca_driver on the 'unit_testing' platform (same mock/stub pattern
 * as tests/dw1000_ranging/); it cannot verify real RF timing or that a real
 * clock-master anchor's SYNC frame is correctly received. Before relying on
 * this module on a DWM1001 tag:
 *
 *   1. Have a real clock-master anchor (or a second DWM1001 board standing in
 *      for one, transmitting a hand-built SYNC frame per uwb_sync_frame_t at
 *      a known cadence) broadcasting SYNC frames.
 *   2. Tag board: call dw1000_sync_rx() with a generous timeout_us covering
 *      at least one full expected cycle period; confirm it returns 0.
 *   3. Confirm out->rx_ts is plausible: non-zero, and consistent from cycle
 *      to cycle (increasing, spaced roughly by the configured superframe
 *      period in DW1000 ticks) -- not a garbage/stuck value.
 *   4. Confirm out->cycle_seq increments by 1 across consecutive received
 *      SYNC frames (accounting for the expected 16-bit wrap).
 *   5. Confirm out->master_tx_ts is a plausible 40-bit timestamp (non-zero,
 *      changing cycle to cycle) -- full ADR-003 offset/drift validation
 *      requires the anchor-side implementation and is out of scope here.
 *   6. Power off the master (or move the tag out of range) and confirm
 *      dw1000_sync_rx() returns -ETIMEDOUT rather than hanging or returning
 *      stale data.
 *
 *   Record board IDs, firmware git SHA, and pass/fail per step in the PR or a
 *   bring-up log -- this module's PR is build-verified only, not
 *   hardware-verified, until this checklist has been run.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef DW1000_SYNC_H_
#define DW1000_SYNC_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parsed result of a successfully received and validated SYNC frame.
 */
typedef struct {
    uint64_t rx_ts;         /**< Tag's local 40-bit DW1000 RX hardware
                                  timestamp of the SYNC frame (already
                                  antenna-delay corrected -- see
                                  dw1000_ranging.h). Coarse cycle-start
                                  reference in the tag's own clock domain. */
    uint16_t cycle_seq;     /**< Superframe/cycle counter decoded from the
                                  frame (UWB_OFF_SYNC_CYCLE_SEQ), wraps
                                  0x0000-0xFFFF. */
    uint64_t master_tx_ts;  /**< Master's 40-bit DW1000 TX timestamp of this
                                  SYNC frame, decoded from the frame
                                  (UWB_OFF_SYNC_MASTER_TX_TS), in the master's
                                  own clock domain. Carried through for the
                                  tracking loop (UWB-243) / a future
                                  clock-alignment consumer -- this module does
                                  not interpret it further. */
} uwb_sync_info_t;

/**
 * @brief Radio-free validate + decode of a raw SYNC frame buffer.
 *
 * Pure function -- no deca_driver / dw1000_rx() dependency, so it is directly
 * unit-testable with hand-built buffers.
 *
 * Validates (in order, against drivers/dw1000/uwb_frames.h constants):
 *   1. @p len == UWB_SYNC_FRAME_SIZE
 *   2. frame_type byte (UWB_OFF_FRAME_TYPE) == UWB_FRAME_TYPE_SYNC
 *   3. PAN ID field (UWB_OFF_PAN_ID) == UWB_PAN_ID
 *   4. Destination address field (UWB_OFF_DEST_ADDR) == UWB_ADDR_BROADCAST
 *
 * On success, decodes:
 *   - @p cycle_seq     from UWB_OFF_SYNC_CYCLE_SEQ (LE uint16)
 *   - @p master_tx_ts  from UWB_OFF_SYNC_MASTER_TX_TS (LE uint40)
 *
 * @param[in]  buf           Raw received frame bytes (PSDU, no FCS).
 * @param[in]  len            Number of valid bytes in @p buf.
 * @param[out] cycle_seq      Set to the decoded cycle sequence on success.
 * @param[out] master_tx_ts   Set to the decoded master TX timestamp on
 *                             success.
 *
 * @return  0         @p buf is a well-formed SYNC frame; outputs are valid.
 * @return  -EINVAL   NULL @p buf / @p cycle_seq / @p master_tx_ts.
 * @return  -EBADMSG  @p buf was well-formed enough to inspect but failed a
 *                     validation check above (wrong length, frame type, PAN
 *                     ID, or destination address).
 */
int uwb_sync_parse(const uint8_t *buf, uint16_t len, uint16_t *cycle_seq,
                    uint64_t *master_tx_ts);

/**
 * @brief Receive one frame via dw1000_rx() and validate/parse it as a SYNC
 *        frame.
 *
 * Calls dw1000_rx() to obtain a frame + hardware RX timestamp, then runs the
 * result through uwb_sync_parse(). Distinguishes "heard nothing" (radio-level
 * timeout/error, passed through unchanged from dw1000_rx()) from "heard the
 * wrong frame" (a frame was received but is not a valid SYNC frame).
 *
 * @param[out] out          Populated with rx_ts / cycle_seq / master_tx_ts on
 *                            success. Untouched on any error.
 * @param      timeout_us   Hardware RX timeout, forwarded to dw1000_rx() (see
 *                            dw1000_ranging.h for units/semantics).
 *
 * @return  0          A valid SYNC frame was received; @p out is populated.
 * @return  -EINVAL    NULL @p out (checked before calling dw1000_rx()).
 * @return  -ETIMEDOUT No frame received before the RX timeout expired
 *                      (passthrough from dw1000_rx()).
 * @return  -EIO       RX error latched by the DW1000 (passthrough from
 *                      dw1000_rx()).
 * @return  -EBADMSG   A frame was received but failed SYNC validation (wrong
 *                      length, frame type, PAN ID, or destination address --
 *                      see uwb_sync_parse()).
 */
int dw1000_sync_rx(uwb_sync_info_t *out, uint32_t timeout_us);

#ifdef __cplusplus
}
#endif

#endif /* DW1000_SYNC_H_ */
