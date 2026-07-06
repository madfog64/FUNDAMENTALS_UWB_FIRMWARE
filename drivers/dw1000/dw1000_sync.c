/*! ----------------------------------------------------------------------------
 * @file    dw1000_sync.c
 * @brief   Tag-side SYNC-frame RX + validation/parse over dw1000_rx (UWB-242).
 *
 * See dw1000_sync.h for the full API doc. Implementation notes:
 *
 *   uwb_sync_parse()   Pure buffer validate/decode -- no deca_driver
 *                        dependency. Length is checked first so every other
 *                        field access below it stays within @p buf's bounds.
 *
 *   dw1000_sync_rx()   Thin composition of dw1000_ranging.c's dw1000_rx()
 *                        (radio RX + hardware RX timestamp) and
 *                        uwb_sync_parse() (validate/decode). The RX buffer is
 *                        sized larger than UWB_SYNC_FRAME_SIZE so that an
 *                        oversized wrong frame is rejected by
 *                        uwb_sync_parse()'s own length check (-EBADMSG)
 *                        rather than by dw1000_rx()'s buffer-capacity guard
 *                        (-EINVAL) -- the caller only ever sees -EBADMSG for
 *                        "heard the wrong frame".
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <errno.h>
#include <stdint.h>

#include "dw1000_ranging.h"
#include "dw1000_sync.h"
#include "uwb_frames.h"

/*
 * RX scratch buffer capacity: large enough to hold any currently-defined
 * frame type (largest today is UWB_TWR_FINAL_FRAME_SIZE, 27 bytes) with
 * headroom, so a wrong/oversized frame is never truncated or rejected by
 * dw1000_rx()'s own capacity guard before uwb_sync_parse() gets a chance to
 * classify it as -EBADMSG.
 */
#define DW1000_SYNC_RX_BUF_SIZE  64u

/* ---------------------------------------------------------------------------
 * Little-endian field decode helpers (buffer offsets, no struct aliasing)
 * --------------------------------------------------------------------------- */
static uint16_t decode_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint64_t decode_le40(const uint8_t *p)
{
    uint64_t ts = 0;
    int i;

    /* p[0] is the LSB, p[4] is the MSB (bits [39:32]) -- matches
     * dw1000_ranging.c's assemble_40bit_timestamp() convention for DW1000
     * 40-bit timestamps and uwb_frames.h's documented wire layout. */
    for (i = 4; i >= 0; i--) {
        ts = (ts << 8) | (uint64_t)p[i];
    }
    return ts;
}

/* ---------------------------------------------------------------------------
 * uwb_sync_parse
 * --------------------------------------------------------------------------- */
int uwb_sync_parse(const uint8_t *buf, uint16_t len, uint16_t *cycle_seq,
                    uint64_t *master_tx_ts)
{
    if (buf == NULL || cycle_seq == NULL || master_tx_ts == NULL) {
        return -EINVAL;
    }

    if (len != UWB_SYNC_FRAME_SIZE) {
        return -EBADMSG;
    }

    if (buf[UWB_OFF_FRAME_TYPE] != (uint8_t)UWB_FRAME_TYPE_SYNC) {
        return -EBADMSG;
    }

    if (decode_le16(&buf[UWB_OFF_PAN_ID]) != UWB_PAN_ID) {
        return -EBADMSG;
    }

    if (decode_le16(&buf[UWB_OFF_DEST_ADDR]) != UWB_ADDR_BROADCAST) {
        return -EBADMSG;
    }

    *cycle_seq    = decode_le16(&buf[UWB_OFF_SYNC_CYCLE_SEQ]);
    *master_tx_ts = decode_le40(&buf[UWB_OFF_SYNC_MASTER_TX_TS]);

    return 0;
}

/* ---------------------------------------------------------------------------
 * dw1000_sync_rx
 * --------------------------------------------------------------------------- */
int dw1000_sync_rx(uwb_sync_info_t *out, uint32_t timeout_us)
{
    uint8_t buf[DW1000_SYNC_RX_BUF_SIZE];
    uint16_t len = sizeof(buf);
    uint64_t rx_ts = 0;
    uint16_t cycle_seq;
    uint64_t master_tx_ts;
    int ret;

    if (out == NULL) {
        return -EINVAL;
    }

    ret = dw1000_rx(buf, &len, &rx_ts, timeout_us);
    if (ret != 0) {
        /* Passthrough: -ETIMEDOUT (heard nothing) / -EIO (RX/FCS error) /
         * -EINVAL (shouldn't happen here -- our buffer is fixed and valid). */
        return ret;
    }

    ret = uwb_sync_parse(buf, len, &cycle_seq, &master_tx_ts);
    if (ret != 0) {
        /* A frame was received but is not a valid SYNC frame -- distinct
         * from "heard nothing" above. */
        return -EBADMSG;
    }

    out->rx_ts = rx_ts;
    out->cycle_seq = cycle_seq;
    out->master_tx_ts = master_tx_ts;

    return 0;
}
