/*! ----------------------------------------------------------------------------
 * @file    uwb_twr_codec.h
 * @brief   Host-testable DS-TWR frame codec (UWB-230).
 *
 * Pure byte manipulation over the pinned uwb_frames.h structs — no hardware,
 * no deca_driver calls.  Builds/parses the DS-TWR POLL/RESPONSE/FINAL frames
 * defined in contracts/uwb/README.md §"DS-TWR calibration frames", giving a
 * fast, deterministic unit-test surface (tests/twr_codec/) for the responder
 * logic implemented elsewhere (UWB-231, UWB-232).
 *
 * All multi-byte fields are little-endian, per contracts/uwb/README.md
 * §"Byte order and bit-numbering conventions".  Nothing here touches the
 * radio, SPI, or timers.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef UWB_TWR_CODEC_H_
#define UWB_TWR_CODEC_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read a 40-bit little-endian DW1000 timestamp from a byte buffer.
 *
 * @param buf  5-byte little-endian buffer (e.g. a *_ts[5] payload field).
 * @return     The timestamp as a 64-bit value, upper 24 bits always zero.
 */
uint64_t uwb_ts_read40(const uint8_t buf[5]);

/**
 * @brief Write a 40-bit little-endian DW1000 timestamp into a byte buffer.
 *
 * @p ts is masked to 2^40-1 before writing — the DW1000 timestamp counter is
 * a 40-bit hardware register that wraps at 2^40; callers must not rely on
 * bits above bit 39 surviving the round trip.
 *
 * @param buf  5-byte destination buffer (e.g. a *_ts[5] payload field).
 * @param ts   Timestamp value; only the low 40 bits are written.
 */
void uwb_ts_write40(uint8_t buf[5], uint64_t ts);

/**
 * @brief Build a complete DS-TWR RESPONSE frame (UWB_FRAME_TYPE_TWR_RESPONSE).
 *
 * Fills the MAC header (Frame Control 0x8841, PAN UWB_PAN_ID, dest =
 * @p initiator_addr, src = @p responder_addr, sequence number @p seq),
 * frame_type = UWB_FRAME_TYPE_TWR_RESPONSE, and the RESPONSE payload
 * (exchange_id, poll_rx_ts = T2, resp_tx_ts = T3), per contracts/uwb/README.md
 * §"DS-TWR calibration frames".
 *
 * @param buf              Destination buffer; must be at least
 *                          UWB_TWR_RESPONSE_FRAME_SIZE bytes.
 * @param exchange_id       Exchange correlation id, copied from the POLL frame.
 * @param initiator_addr    Initiator's 16-bit short address (frame dest_addr).
 * @param responder_addr    Responder's 16-bit short address (frame src_addr).
 * @param seq               MAC sequence number to embed.
 * @param poll_rx_ts         T2 — responder's DW1000 RX timestamp of the POLL.
 * @param resp_tx_ts         T3 — responder's scheduled DW1000 TX timestamp of
 *                          this RESPONSE.
 * @return  UWB_TWR_RESPONSE_FRAME_SIZE (22) on success; negative errno if
 *          @p buf is NULL.
 */
int uwb_build_twr_response(uint8_t *buf,
                            uint16_t exchange_id,
                            uint16_t initiator_addr,
                            uint16_t responder_addr,
                            uint8_t seq,
                            uint64_t poll_rx_ts,
                            uint64_t resp_tx_ts);

/**
 * @brief Parse and validate a DS-TWR POLL frame (UWB_FRAME_TYPE_TWR_POLL).
 *
 * Validates the Frame Control word, PAN ID, frame_type, and buffer length
 * before extracting fields; rejects malformed frames with a negative errno
 * rather than reading out of bounds.
 *
 * @param buf              Received frame buffer.
 * @param len              Number of valid bytes in @p buf.
 * @param[out] initiator_addr  Set to the frame's src_addr (initiator). May be NULL.
 * @param[out] exchange_id      Set to the POLL payload's exchange_id. May be NULL.
 * @return  0 on success.
 * @return  -EINVAL  @p buf is NULL.
 * @return  -EMSGSIZE  @p len != UWB_TWR_POLL_FRAME_SIZE.
 * @return  -EPROTO  Frame Control or PAN ID mismatch, or frame_type is not
 *                    UWB_FRAME_TYPE_TWR_POLL.
 */
int uwb_parse_twr_poll(const uint8_t *buf, size_t len,
                        uint16_t *initiator_addr, uint16_t *exchange_id);

/**
 * @brief Parse and validate a DS-TWR FINAL frame (UWB_FRAME_TYPE_TWR_FINAL).
 *
 * Validates the Frame Control word, PAN ID, frame_type, and buffer length
 * before extracting fields; rejects malformed frames with a negative errno
 * rather than reading out of bounds.
 *
 * @param buf              Received frame buffer.
 * @param len              Number of valid bytes in @p buf.
 * @param[out] exchange_id  Set to the FINAL payload's exchange_id. May be NULL.
 * @param[out] poll_tx_ts    Set to T1 (initiator's POLL TX timestamp). May be NULL.
 * @param[out] resp_rx_ts    Set to T4 (initiator's RESPONSE RX timestamp). May be NULL.
 * @param[out] final_tx_ts   Set to T5 (initiator's scheduled FINAL TX timestamp).
 *                          May be NULL.
 * @return  0 on success.
 * @return  -EINVAL  @p buf is NULL.
 * @return  -EMSGSIZE  @p len != UWB_TWR_FINAL_FRAME_SIZE.
 * @return  -EPROTO  Frame Control or PAN ID mismatch, or frame_type is not
 *                    UWB_FRAME_TYPE_TWR_FINAL.
 */
int uwb_parse_twr_final(const uint8_t *buf, size_t len,
                         uint16_t *exchange_id,
                         uint64_t *poll_tx_ts,
                         uint64_t *resp_rx_ts,
                         uint64_t *final_tx_ts);

#ifdef __cplusplus
}
#endif

#endif /* UWB_TWR_CODEC_H_ */
