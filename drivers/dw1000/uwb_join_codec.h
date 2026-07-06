/*! ----------------------------------------------------------------------------
 * @file    uwb_join_codec.h
 * @brief   Host-testable Aloha join frame codec (UWB-259).
 *
 * Pure byte manipulation over the pinned uwb_frames.h structs — no hardware,
 * no deca_driver calls.  Builds a JOIN_REQUEST (UWB_FRAME_TYPE_JOIN_REQUEST,
 * 0x20) and parses a SLOT_ASSIGNMENT (UWB_FRAME_TYPE_SLOT_ASSIGNMENT, 0x21),
 * per contracts/uwb v0.6 and ADR-0039 (registrar + tag join state machine).
 * Gives a fast, deterministic unit-test surface (tests/join_codec/) for the
 * tag-side join state machine implemented elsewhere (UWB-261).
 *
 * All multi-byte fields are little-endian, per contracts/uwb/README.md
 * §"Byte order and bit-numbering conventions".  Nothing here touches the
 * radio, SPI, timers, or any retry/backoff state — that is the join state
 * machine's concern (UWB-261), not this codec's.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef UWB_JOIN_CODEC_H_
#define UWB_JOIN_CODEC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Length of an EUI-64 in bytes.
 */
#define UWB_EUI64_LEN  8u

/**
 * @brief Result of parsing a SLOT_ASSIGNMENT frame (UWB-259).
 *
 * Mirrors the wire fields of uwb_slot_assignment_payload_t
 * (drivers/dw1000/uwb_frames.h) in host byte order (native endianness) rather
 * than the on-wire little-endian byte arrays.
 */
typedef struct {
    uint8_t  target_eui64[UWB_EUI64_LEN]; /**< Target tag's EUI-64, LE byte
                                                order preserved (LSB at
                                                index 0) so it can be compared
                                                byte-for-byte against a tag's
                                                own EUI-64 buffer.            */
    uint16_t short_addr;                  /**< Assigned 16-bit short address. */
    uint8_t  slot_idx;                     /**< Assigned TDMA slot index.     */
    uint8_t  slot_count;                   /**< Total slots in the superframe. */
    uint16_t slot_duration_us;             /**< Slot duration, microseconds.  */
} uwb_slot_assignment_t;

/**
 * @brief Build a complete JOIN_REQUEST frame (UWB_FRAME_TYPE_JOIN_REQUEST).
 *
 * Fills the MAC header (Frame Control 0x8841, PAN UWB_PAN_ID,
 * dest = UWB_ADDR_BROADCAST, src = UWB_ADDR_UNASSIGNED, sequence number
 * @p seq), frame_type = UWB_FRAME_TYPE_JOIN_REQUEST, and the join payload
 * (eui64, capabilities), per contracts/uwb/README.md §"Join request frame".
 *
 * Reserved capability bits (7:1) are always written as 0, regardless of the
 * bits set in @p capabilities.
 *
 * The MAC sequence number is a build parameter — this module owns no
 * sequence counter; the caller (the tag's join state machine, UWB-261) is
 * responsible for incrementing it per transmitted frame.  Likewise, the
 * EUI-64 is caller-supplied — reading it from the DW1000 EUI-64 register is
 * a bring-up/state-machine concern, not this codec's.
 *
 * @param buf           Destination buffer; must be at least
 *                        UWB_JOIN_REQUEST_FRAME_SIZE bytes.
 * @param eui64          Tag's 64-bit DW1000 extended address, little-endian
 *                        byte array (UWB_EUI64_LEN bytes, LSB at index 0).
 * @param seq            MAC sequence number to embed.
 * @param capabilities   Role/capability flags (e.g. UWB_JOIN_CAP_ROLE_REFERENCE);
 *                        reserved bits are masked to 0 before writing.
 * @return  UWB_JOIN_REQUEST_FRAME_SIZE (19) on success; negative errno if
 *          @p buf or @p eui64 is NULL.
 */
int uwb_build_join_request(uint8_t *buf,
                            const uint8_t eui64[UWB_EUI64_LEN],
                            uint8_t seq,
                            uint8_t capabilities);

/**
 * @brief Parse and validate a SLOT_ASSIGNMENT frame (UWB_FRAME_TYPE_SLOT_ASSIGNMENT).
 *
 * Validates the buffer length and frame_type before extracting fields;
 * rejects malformed frames with a negative errno rather than reading out of
 * bounds.  Wrong length and wrong frame_type are reported with distinct,
 * non-zero (negative) error codes.
 *
 * @param buf           Received frame buffer.
 * @param len           Number of valid bytes in @p buf.
 * @param[out] out       Set to the parsed fields on success. Must not be NULL.
 * @return  0 on success.
 * @return  -EINVAL    @p buf or @p out is NULL.
 * @return  -EMSGSIZE  @p len != UWB_SLOT_ASSIGNMENT_FRAME_SIZE.
 * @return  -EPROTO    frame_type is not UWB_FRAME_TYPE_SLOT_ASSIGNMENT.
 */
int uwb_parse_slot_assignment(const uint8_t *buf, size_t len,
                               uwb_slot_assignment_t *out);

/**
 * @brief Self-select rule: is this SLOT_ASSIGNMENT addressed to me?
 *
 * Pure comparison of @p parsed->target_eui64 against the tag's own EUI-64
 * (ADR-0039: every listening tag checks the target_eui64 payload field
 * against its own EUI-64 and ignores the frame if it does not match).
 * Carries no radio/timing state — purely a byte-compare.
 *
 * @param eui64_self  This tag's own 64-bit DW1000 extended address,
 *                     little-endian byte array (UWB_EUI64_LEN bytes).
 * @param parsed       A successfully-parsed SLOT_ASSIGNMENT (see
 *                     uwb_parse_slot_assignment()). Must not be NULL.
 * @return  true iff @p eui64_self exactly matches @p parsed->target_eui64.
 *          false if @p eui64_self or @p parsed is NULL, or on any mismatch.
 */
bool uwb_join_assignment_is_for_me(const uint8_t eui64_self[UWB_EUI64_LEN],
                                    const uwb_slot_assignment_t *parsed);

#ifdef __cplusplus
}
#endif

#endif /* UWB_JOIN_CODEC_H_ */
