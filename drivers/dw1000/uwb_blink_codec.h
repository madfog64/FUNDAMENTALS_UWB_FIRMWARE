/*! ----------------------------------------------------------------------------
 * @file    uwb_blink_codec.h
 * @brief   Host-testable TDoA tag-blink frame codec (UWB-250).
 *
 * Pure byte manipulation over the pinned uwb_frames.h structs — no hardware,
 * no deca_driver calls.  Builds/parses the TDoA tag blink frame defined in
 * contracts/uwb/README.md §"TDoA tag blink frame", giving a fast,
 * deterministic unit-test surface (tests/blink_codec/) for the slot-timing
 * and tracking-loop logic implemented elsewhere (UWB-251, UWB-252).
 *
 * All multi-byte fields are little-endian, per contracts/uwb/README.md
 * §"Byte order and bit-numbering conventions".  Nothing here touches the
 * radio, SPI, or timers.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef UWB_BLINK_CODEC_H_
#define UWB_BLINK_CODEC_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build a complete TDoA tag blink frame (UWB_FRAME_TYPE_TAG_BLINK).
 *
 * Fills the MAC header (Frame Control 0x8841, PAN UWB_PAN_ID, dest =
 * UWB_ADDR_BROADCAST, src = @p src_addr, sequence number @p seq),
 * frame_type = UWB_FRAME_TYPE_TAG_BLINK, and the blink payload (blink_count,
 * flags), per contracts/uwb/README.md §"TDoA tag blink frame".
 *
 * Reserved flag bits (7:1) are always written as 0, regardless of the bits
 * set in @p flags.
 *
 * The MAC sequence number is a build parameter — this module owns no
 * sequence counter; the caller (e.g. the tag's TX scheduling logic, UWB-251)
 * is responsible for incrementing it per transmitted frame.
 *
 * @param buf          Destination buffer; must be at least
 *                       UWB_TAG_BLINK_FRAME_SIZE bytes.
 * @param src_addr      Tag's 16-bit short address (frame src_addr).
 * @param seq           MAC sequence number to embed.
 * @param blink_count   Per-tag 16-bit blink counter to embed, little-endian.
 * @param flags         Status flags byte (e.g. UWB_BLINK_FLAG_LOW_BATTERY);
 *                       reserved bits are masked to 0 before writing.
 * @return  UWB_TAG_BLINK_FRAME_SIZE (13) on success; negative errno if
 *          @p buf is NULL.
 */
int uwb_build_tag_blink(uint8_t *buf,
                         uint16_t src_addr,
                         uint8_t seq,
                         uint16_t blink_count,
                         uint8_t flags);

/**
 * @brief Parse and validate a TDoA tag blink frame (UWB_FRAME_TYPE_TAG_BLINK).
 *
 * Validates the buffer length and frame_type before extracting fields;
 * rejects malformed frames with a negative errno rather than reading out of
 * bounds.
 *
 * @param buf              Received frame buffer.
 * @param len              Number of valid bytes in @p buf.
 * @param[out] src_addr     Set to the frame's src_addr (tag). May be NULL.
 * @param[out] blink_count  Set to the payload's blink_count. May be NULL.
 * @param[out] flags        Set to the payload's flags byte. May be NULL.
 * @return  0 on success.
 * @return  -EINVAL    @p buf is NULL.
 * @return  -EMSGSIZE  @p len != UWB_TAG_BLINK_FRAME_SIZE.
 * @return  -EPROTO    frame_type is not UWB_FRAME_TYPE_TAG_BLINK.
 */
int uwb_parse_tag_blink(const uint8_t *buf, size_t len,
                         uint16_t *src_addr,
                         uint16_t *blink_count,
                         uint8_t *flags);

#ifdef __cplusplus
}
#endif

#endif /* UWB_BLINK_CODEC_H_ */
