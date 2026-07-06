/*! ----------------------------------------------------------------------------
 * @file    uwb_blink_codec.c
 * @brief   Host-testable TDoA tag-blink frame codec (UWB-250).
 *
 * Implements build/parse functions for the TDoA tag blink frame defined in
 * drivers/dw1000/uwb_frames.h and contracts/uwb/README.md §"TDoA tag blink
 * frame".  Pure byte manipulation — no SPI, no deca_driver, no radio access —
 * so this module builds and runs on the unit_testing platform
 * (tests/blink_codec/).
 *
 * All field offsets come from the UWB_OFF_* constants and uwb_frames.h struct
 * layouts; no magic numbers are used here.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <errno.h>

#include "uwb_frames.h"
#include "uwb_blink_codec.h"

/* ---------------------------------------------------------------------------
 * Internal helpers — little-endian uint16 read/write
 * --------------------------------------------------------------------------- */

static void write_le16(uint8_t out[2], uint16_t v)
{
    out[0] = (uint8_t)(v & 0xFFU);
    out[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static uint16_t read_le16(const uint8_t in[2])
{
    return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

/* ---------------------------------------------------------------------------
 * Build
 * --------------------------------------------------------------------------- */

int uwb_build_tag_blink(uint8_t *buf,
                         uint16_t src_addr,
                         uint8_t seq,
                         uint16_t blink_count,
                         uint8_t flags)
{
    if (buf == NULL) {
        return -EINVAL;
    }

    uwb_tag_blink_frame_t *frame = (uwb_tag_blink_frame_t *)buf;

    frame->hdr.mac.frame_ctrl[0] = UWB_FRAME_CTRL_LOW;
    frame->hdr.mac.frame_ctrl[1] = UWB_FRAME_CTRL_HIGH;
    frame->hdr.mac.seq_num = seq;
    write_le16(frame->hdr.mac.pan_id, UWB_PAN_ID);
    write_le16(frame->hdr.mac.dest_addr, UWB_ADDR_BROADCAST);
    write_le16(frame->hdr.mac.src_addr, src_addr);
    frame->hdr.frame_type = (uint8_t)UWB_FRAME_TYPE_TAG_BLINK;

    write_le16(frame->payload.blink_count, blink_count);

    /* Reserved bits (7:1) must transmit as 0 — mask to the only defined
     * flag bit regardless of what the caller passed in. */
    frame->payload.flags = (uint8_t)(flags & UWB_BLINK_FLAG_LOW_BATTERY);

    return (int)UWB_TAG_BLINK_FRAME_SIZE;
}

/* ---------------------------------------------------------------------------
 * Parse
 * --------------------------------------------------------------------------- */

int uwb_parse_tag_blink(const uint8_t *buf, size_t len,
                         uint16_t *src_addr,
                         uint16_t *blink_count,
                         uint8_t *flags)
{
    if (buf == NULL) {
        return -EINVAL;
    }

    if (len != UWB_TAG_BLINK_FRAME_SIZE) {
        return -EMSGSIZE;
    }

    const uwb_tag_blink_frame_t *frame = (const uwb_tag_blink_frame_t *)buf;

    if (frame->hdr.frame_type != (uint8_t)UWB_FRAME_TYPE_TAG_BLINK) {
        return -EPROTO;
    }

    if (src_addr != NULL) {
        *src_addr = read_le16(frame->hdr.mac.src_addr);
    }

    if (blink_count != NULL) {
        *blink_count = read_le16(frame->payload.blink_count);
    }

    if (flags != NULL) {
        *flags = frame->payload.flags;
    }

    return 0;
}
