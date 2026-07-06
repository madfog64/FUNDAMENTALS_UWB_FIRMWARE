/*! ----------------------------------------------------------------------------
 * @file    uwb_join_codec.c
 * @brief   Host-testable Aloha join frame codec (UWB-259).
 *
 * Implements build/parse functions for the JOIN_REQUEST and SLOT_ASSIGNMENT
 * frames defined in drivers/dw1000/uwb_frames.h and contracts/uwb/README.md
 * §"Join request frame" / §"Slot / address assignment frame" (v0.6). Pure
 * byte manipulation — no SPI, no deca_driver, no radio access — so this
 * module builds and runs on the unit_testing platform (tests/join_codec/).
 *
 * All field offsets come from the UWB_OFF_* constants and uwb_frames.h struct
 * layouts; no magic numbers are used here.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <errno.h>
#include <string.h>

#include "uwb_frames.h"
#include "uwb_join_codec.h"

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
 * Build — JOIN_REQUEST (UWB_FRAME_TYPE_JOIN_REQUEST, 0x20)
 * --------------------------------------------------------------------------- */

int uwb_build_join_request(uint8_t *buf,
                            const uint8_t eui64[UWB_EUI64_LEN],
                            uint8_t seq,
                            uint8_t capabilities)
{
    if (buf == NULL || eui64 == NULL) {
        return -EINVAL;
    }

    uwb_join_request_frame_t *frame = (uwb_join_request_frame_t *)buf;

    frame->hdr.mac.frame_ctrl[0] = UWB_FRAME_CTRL_LOW;
    frame->hdr.mac.frame_ctrl[1] = UWB_FRAME_CTRL_HIGH;
    frame->hdr.mac.seq_num = seq;
    write_le16(frame->hdr.mac.pan_id, UWB_PAN_ID);
    write_le16(frame->hdr.mac.dest_addr, UWB_ADDR_BROADCAST);
    write_le16(frame->hdr.mac.src_addr, UWB_ADDR_UNASSIGNED);
    frame->hdr.frame_type = (uint8_t)UWB_FRAME_TYPE_JOIN_REQUEST;

    memcpy(frame->payload.eui64, eui64, UWB_EUI64_LEN);

    /* Reserved bits (7:1) must transmit as 0 — mask to the only defined
     * capability bit regardless of what the caller passed in. */
    frame->payload.capabilities = (uint8_t)(capabilities & UWB_JOIN_CAP_ROLE_REFERENCE);

    return (int)UWB_JOIN_REQUEST_FRAME_SIZE;
}

/* ---------------------------------------------------------------------------
 * Parse — SLOT_ASSIGNMENT (UWB_FRAME_TYPE_SLOT_ASSIGNMENT, 0x21)
 * --------------------------------------------------------------------------- */

int uwb_parse_slot_assignment(const uint8_t *buf, size_t len,
                               uwb_slot_assignment_t *out)
{
    if (buf == NULL || out == NULL) {
        return -EINVAL;
    }

    if (len != UWB_SLOT_ASSIGNMENT_FRAME_SIZE) {
        return -EMSGSIZE;
    }

    const uwb_slot_assignment_frame_t *frame = (const uwb_slot_assignment_frame_t *)buf;

    if (frame->hdr.frame_type != (uint8_t)UWB_FRAME_TYPE_SLOT_ASSIGNMENT) {
        return -EPROTO;
    }

    memcpy(out->target_eui64, frame->payload.target_eui64, UWB_EUI64_LEN);
    out->short_addr = read_le16(frame->payload.short_addr);
    out->slot_idx = frame->payload.slot_idx;
    out->slot_count = frame->payload.slot_count;
    out->slot_duration_us = read_le16(frame->payload.slot_duration_us);

    return 0;
}

/* ---------------------------------------------------------------------------
 * Self-select rule
 * --------------------------------------------------------------------------- */

bool uwb_join_assignment_is_for_me(const uint8_t eui64_self[UWB_EUI64_LEN],
                                    const uwb_slot_assignment_t *parsed)
{
    if (eui64_self == NULL || parsed == NULL) {
        return false;
    }

    return memcmp(eui64_self, parsed->target_eui64, UWB_EUI64_LEN) == 0;
}
