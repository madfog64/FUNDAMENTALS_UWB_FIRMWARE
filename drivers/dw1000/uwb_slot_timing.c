/*! ----------------------------------------------------------------------------
 * @file    uwb_slot_timing.c
 * @brief   TDMA slot-timing computation for tag blink TX (UWB-251). See
 *          uwb_slot_timing.h for the full API doc and scope note.
 *
 * Pure arithmetic -- no deca_driver calls of its own, no radio, no timers.
 * The only external call is dw1000_delayed_tx_time() (UWB-231), which does
 * the actual low-9-bit truncation this module deliberately does not
 * re-implement.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "dw1000_ranging.h"
#include "uwb_slot_timing.h"

/*
 * dw1000_delayed_tx_time()'s delay_dtu parameter is a plain uint32_t (see
 * dw1000_ranging.h) -- e.g. the same width used elsewhere in this codebase
 * for a reply delay (CONFIG_UWB_TWR_RESP_REPLY_DELAY_DTU). A slot delay
 * (slot_idx * slot_duration_dtu) that does not fit in 32 bits would silently
 * truncate when narrowed to that parameter, producing a wrapped/garbage TX
 * time rather than a clear error -- exactly the failure mode this guard
 * exists to prevent. This is a tighter (and correct, given the driver's
 * signature) bound than the DW1000's 40-bit system-time counter: the 40-bit
 * counter only bounds the *sum* (sync_rx_ts + delay), which
 * dw1000_delayed_tx_time() already wraps modulo 2^40 by design; the delay
 * operand itself must additionally fit in the 32 bits the driver accepts.
 */
#define UWB_SLOT_TIMING_MAX_DELAY_DTU  ((uint64_t)0xFFFFFFFFULL)

int uwb_slot_tx_time(uint64_t sync_rx_ts, uint32_t slot_idx,
                      uint32_t slot_duration_dtu, uint32_t slot_count,
                      uint64_t *tx_time_dtu)
{
    uint64_t delay_dtu;

    if (tx_time_dtu == NULL) {
        return -EINVAL;
    }

    if (slot_idx >= slot_count) {
        return -EINVAL;
    }

    /* uint32_t * uint32_t promoted to uint64_t: the largest possible product
     * (0xFFFFFFFF * 0xFFFFFFFF ~= 1.8446744065e19) is comfortably below
     * UINT64_MAX (~1.8446744074e19), so this multiplication itself can never
     * overflow -- the range check below is what catches a delay too large to
     * hand to dw1000_delayed_tx_time(). */
    delay_dtu = (uint64_t)slot_idx * (uint64_t)slot_duration_dtu;

    if (delay_dtu > UWB_SLOT_TIMING_MAX_DELAY_DTU) {
        return -ERANGE;
    }

    *tx_time_dtu = dw1000_delayed_tx_time(sync_rx_ts, (uint32_t)delay_dtu);

    return 0;
}
