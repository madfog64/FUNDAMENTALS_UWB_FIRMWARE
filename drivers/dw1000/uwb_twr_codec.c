/*! ----------------------------------------------------------------------------
 * @file    uwb_twr_codec.c
 * @brief   Host-testable DS-TWR frame codec (UWB-230).
 *
 * Implements uint40 little-endian timestamp helpers plus build/parse
 * functions for the three DS-TWR calibration frames (POLL/RESPONSE/FINAL)
 * defined in drivers/dw1000/uwb_frames.h and contracts/uwb/README.md
 * §"DS-TWR calibration frames".  Pure byte manipulation — no SPI, no
 * deca_driver, no radio access — so this module builds and runs on the
 * unit_testing platform (tests/twr_codec/).
 *
 * All field offsets come from the UWB_OFF_TWR_* constants and uwb_frames.h
 * struct layouts; no magic numbers are used here.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <errno.h>

#include "uwb_frames.h"
#include "uwb_twr_codec.h"

/* ---------------------------------------------------------------------------
 * uint40 little-endian helpers
 * --------------------------------------------------------------------------- */

uint64_t uwb_ts_read40(const uint8_t buf[5])
{
    uint64_t ts = 0;

    for (int i = 4; i >= 0; i--) {
        ts = (ts << 8) | (uint64_t)buf[i];
    }

    return ts;
}

void uwb_ts_write40(uint8_t buf[5], uint64_t ts)
{
    uint64_t masked = ts & 0xFFFFFFFFFFULL; /* mask to 2^40 - 1 */

    for (int i = 0; i < 5; i++) {
        buf[i] = (uint8_t)(masked & 0xFFU);
        masked >>= 8;
    }
}

/* ---------------------------------------------------------------------------
 * Internal helpers — MAC header fill / validate
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

static void fill_mac_hdr(uwb_mac_hdr_t *mac, uint16_t dest_addr,
                          uint16_t src_addr, uint8_t seq)
{
    mac->frame_ctrl[0] = UWB_FRAME_CTRL_LOW;
    mac->frame_ctrl[1] = UWB_FRAME_CTRL_HIGH;
    mac->seq_num = seq;
    write_le16(mac->pan_id, UWB_PAN_ID);
    write_le16(mac->dest_addr, dest_addr);
    write_le16(mac->src_addr, src_addr);
}

/**
 * @brief Validate the common MAC header + frame_type of a received TWR frame.
 *
 * @return  0 on success; -EPROTO if Frame Control, PAN ID, or frame_type
 *          do not match expectations.
 */
static int validate_twr_hdr(const uwb_app_hdr_t *hdr, uint8_t expected_frame_type)
{
    if (hdr->mac.frame_ctrl[0] != UWB_FRAME_CTRL_LOW ||
        hdr->mac.frame_ctrl[1] != UWB_FRAME_CTRL_HIGH) {
        return -EPROTO;
    }

    if (read_le16(hdr->mac.pan_id) != UWB_PAN_ID) {
        return -EPROTO;
    }

    if (hdr->frame_type != expected_frame_type) {
        return -EPROTO;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Build
 * --------------------------------------------------------------------------- */

int uwb_build_twr_response(uint8_t *buf,
                            uint16_t exchange_id,
                            uint16_t initiator_addr,
                            uint16_t responder_addr,
                            uint8_t seq,
                            uint64_t poll_rx_ts,
                            uint64_t resp_tx_ts)
{
    if (buf == NULL) {
        return -EINVAL;
    }

    uwb_twr_resp_frame_t *frame = (uwb_twr_resp_frame_t *)buf;

    fill_mac_hdr(&frame->hdr.mac, initiator_addr, responder_addr, seq);
    frame->hdr.frame_type = (uint8_t)UWB_FRAME_TYPE_TWR_RESPONSE;

    write_le16(frame->payload.exchange_id, exchange_id);
    uwb_ts_write40(frame->payload.poll_rx_ts, poll_rx_ts);
    uwb_ts_write40(frame->payload.resp_tx_ts, resp_tx_ts);

    return (int)UWB_TWR_RESPONSE_FRAME_SIZE;
}

/* ---------------------------------------------------------------------------
 * Parse
 * --------------------------------------------------------------------------- */

int uwb_parse_twr_poll(const uint8_t *buf, size_t len,
                        uint16_t *initiator_addr, uint16_t *exchange_id)
{
    if (buf == NULL) {
        return -EINVAL;
    }

    if (len != UWB_TWR_POLL_FRAME_SIZE) {
        return -EMSGSIZE;
    }

    const uwb_twr_poll_frame_t *frame = (const uwb_twr_poll_frame_t *)buf;

    int ret = validate_twr_hdr(&frame->hdr, (uint8_t)UWB_FRAME_TYPE_TWR_POLL);
    if (ret != 0) {
        return ret;
    }

    if (initiator_addr != NULL) {
        *initiator_addr = read_le16(frame->hdr.mac.src_addr);
    }

    if (exchange_id != NULL) {
        *exchange_id = read_le16(frame->payload.exchange_id);
    }

    return 0;
}

int uwb_parse_twr_final(const uint8_t *buf, size_t len,
                         uint16_t *exchange_id,
                         uint64_t *poll_tx_ts,
                         uint64_t *resp_rx_ts,
                         uint64_t *final_tx_ts)
{
    if (buf == NULL) {
        return -EINVAL;
    }

    if (len != UWB_TWR_FINAL_FRAME_SIZE) {
        return -EMSGSIZE;
    }

    const uwb_twr_final_frame_t *frame = (const uwb_twr_final_frame_t *)buf;

    int ret = validate_twr_hdr(&frame->hdr, (uint8_t)UWB_FRAME_TYPE_TWR_FINAL);
    if (ret != 0) {
        return ret;
    }

    if (exchange_id != NULL) {
        *exchange_id = read_le16(frame->payload.exchange_id);
    }

    if (poll_tx_ts != NULL) {
        *poll_tx_ts = uwb_ts_read40(frame->payload.poll_tx_ts);
    }

    if (resp_rx_ts != NULL) {
        *resp_rx_ts = uwb_ts_read40(frame->payload.resp_rx_ts);
    }

    if (final_tx_ts != NULL) {
        *final_tx_ts = uwb_ts_read40(frame->payload.final_tx_ts);
    }

    return 0;
}
