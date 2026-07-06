/*! ----------------------------------------------------------------------------
 * @file    uwb_slot_timing.h
 * @brief   TDMA slot-timing computation for tag blink TX (UWB-251).
 *
 * ADR-001 §Phase 2: a tag transmits its blink at
 *
 *     slot_tx_time = sync_rx_ts + slot_idx * slot_duration_dtu
 *
 * where @c sync_rx_ts is the tag's own local RX timestamp of the master's
 * SYNC frame (see uwb_cycle_ref_get(), UWB-243) and @c slot_idx is this tag's
 * registration-assigned TDMA slot index. TDoA cancels the tag's unknown TX
 * time (positioning precision comes entirely from the anchors' RX
 * timestamps), so this computation only needs **collision-avoidance**
 * accuracy -- land inside the assigned slot, clear of guard bands -- not
 * nanosecond precision.
 *
 * This module is the correctness crux of that formula, isolated from the
 * radio loop: it validates its inputs and defers the actual truncation to
 * dw1000_delayed_tx_time() (UWB-231), which already knows the DW1000's
 * low-9-bit delayed-TX/RX scheduling granularity. This module does NOT
 * re-implement that truncation.
 *
 * -----------------------------------------------------------------------
 * Guard time lives in slot_duration_dtu
 * -----------------------------------------------------------------------
 * @c slot_duration_dtu is the FULL per-slot duration, INCLUSIVE of the guard
 * band (ADR-001: superframe ~24 slots, slot ~0.5 ms ~= ~150 us blink + guard).
 * This module places the computed TX time at the slot's start boundary
 * (slot_idx * slot_duration_dtu offset from the SYNC edge); the tunable
 * guard-band knob is entirely folded into the value the caller chooses for
 * @c slot_duration_dtu -- there is no separate guard parameter here.
 *
 * -----------------------------------------------------------------------
 * Out of scope (see UWB-252 / UWB-11)
 * -----------------------------------------------------------------------
 * This module does NOT read current radio time, decide whether a slot has
 * already passed, read the SYNC frame, update the cycle reference, or
 * schedule the TX. It is pure arithmetic over caller-supplied values. "Slot
 * already passed" is a live-hardware-time question, handled by the tracking
 * loop (UWB-252) via dw1000_tx_at()'s -EIO return. Slot assignment / the
 * provenance of @c slot_idx is a registration concern (UWB-11), injected here
 * as a plain parameter.
 *
 * -----------------------------------------------------------------------
 * On-hardware bring-up
 * -----------------------------------------------------------------------
 * None needed beyond dw1000_delayed_tx_time()'s own bring-up checklist (see
 * dw1000_ranging.h) -- this module adds no new radio behaviour, only the
 * validated slot-index arithmetic that feeds into it. It is exercised
 * entirely by tests/slot_timing/ on the 'unit_testing' platform.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef UWB_SLOT_TIMING_H_
#define UWB_SLOT_TIMING_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compute the scheduled blink TX time for a tag's assigned TDMA slot,
 *        referenced off the tag's local SYNC RX timestamp.
 *
 * Implements slot_tx_time = sync_rx_ts + slot_idx * slot_duration_dtu
 * (ADR-001 §Phase 2) by validating the inputs and then calling
 * dw1000_delayed_tx_time(sync_rx_ts, slot_idx * slot_duration_dtu) (UWB-231)
 * so the low-9-bit delayed-TX truncation is applied exactly once, in the one
 * place that already knows how to do it correctly.
 *
 * @param sync_rx_ts         Tag's local RX timestamp of the most recent valid
 *                            SYNC frame (40-bit DW1000 DTU) -- see
 *                            uwb_cycle_ref_get().
 * @param slot_idx            This tag's registration-assigned TDMA slot index
 *                            (0-based; UWB-11 provenance, injected here).
 * @param slot_duration_dtu   Per-slot duration in DW1000 time units,
 *                            INCLUSIVE of the guard band -- see the "Guard
 *                            time" section in this header's top-of-file
 *                            comment.
 * @param slot_count          Number of slots in the current superframe;
 *                            bounds @p slot_idx.
 * @param[out] tx_time_dtu    Set to the scheduled (already truncated) TX
 *                            time on success, ready to pass as @c tx_time_dtu
 *                            to dw1000_tx_at(). Untouched on error.
 *
 * @return  0        Success; @p *tx_time_dtu is valid.
 * @return  -EINVAL  @p tx_time_dtu is NULL, or @p slot_idx >= @p slot_count.
 *                    dw1000_delayed_tx_time() is NOT called in this case.
 * @return  -ERANGE  @p slot_idx * @p slot_duration_dtu exceeds the range a
 *                    delay can be represented in (dw1000_delayed_tx_time()'s
 *                    @c delay_dtu parameter is 32 bits wide -- see the
 *                    implementation note in uwb_slot_timing.c). Returned
 *                    instead of silently passing a truncated/wrapped delay
 *                    into the driver. dw1000_delayed_tx_time() is NOT called
 *                    in this case.
 */
int uwb_slot_tx_time(uint64_t sync_rx_ts, uint32_t slot_idx,
                      uint32_t slot_duration_dtu, uint32_t slot_count,
                      uint64_t *tx_time_dtu);

#ifdef __cplusplus
}
#endif

#endif /* UWB_SLOT_TIMING_H_ */
