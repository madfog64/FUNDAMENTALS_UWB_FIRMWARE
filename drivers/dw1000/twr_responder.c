/*! ----------------------------------------------------------------------------
 * @file    twr_responder.c
 * @brief   DS-TWR responder mode state machine (UWB-232).
 *
 * See twr_responder.h for the full API doc. Implementation notes:
 *
 *   twr_responder_run_once()   Composes dw1000_ranging.c's dw1000_rx() /
 *                               dw1000_tx_at() / dw1000_delayed_tx_time()
 *                               (UWB-231) with uwb_twr_codec.c's
 *                               uwb_build_twr_response() / uwb_parse_twr_poll()
 *                               / uwb_parse_twr_final() (UWB-230). Every RX
 *                               wait is bounded by a Kconfig timeout, so this
 *                               function always returns rather than blocking
 *                               forever — callers loop by calling it again.
 *
 *   uwb_twr_range_mm()          Pure DS-TWR range formula (no hardware
 *                               dependency); only invoked internally when
 *                               CONFIG_UWB_TWR_RESPONDER_DEBUG_RANGE=y, but
 *                               always compiled so it is directly testable.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include "dw1000_ranging.h"
#include "twr_responder.h"
#include "uwb_frames.h"
#include "uwb_twr_codec.h"

LOG_MODULE_REGISTER(twr_responder, LOG_LEVEL_DBG);

/*
 * RX scratch buffer: large enough for the biggest DS-TWR frame this module
 * receives (FINAL, UWB_TWR_FINAL_FRAME_SIZE = 27 bytes) with headroom, same
 * sizing rationale as dw1000_sync.c's DW1000_SYNC_RX_BUF_SIZE.
 */
#define TWR_RESPONDER_RX_BUF_SIZE  64u

/*
 * dw1000_tx_at()'s 'len' parameter follows deca_driver convention: the TOTAL
 * frame length INCLUDING the 2-byte CRC the DW1000 hardware appends
 * automatically on TX (the buffer itself must NOT contain the CRC bytes) --
 * see dw1000_ranging.h's dw1000_tx_at() doc comment.
 */
#define TWR_RESPONDER_CRC_LEN  2u

/* DW1000 system time counter is 40 bits wide (matches dw1000_ranging.c). */
#define TWR_RESPONDER_TIME_MASK_40BIT  ((uint64_t)0xFFFFFFFFFFULL)

/* 1 DW1000 tick ~= 4.691763 mm one-way path (uwb_frames.h §"DS-TWR
 * calibration frames" range formula: distance[m] = ToF x 0.004691763 m/tick). */
#define TWR_RESPONDER_MM_PER_TICK  4.691763

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

/** Per-device MAC sequence number for RESPONSE frames this responder sends. */
static uint8_t next_seq(void)
{
    static uint8_t seq;

    return seq++;
}

static uint16_t decode_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ---------------------------------------------------------------------------
 * uwb_twr_range_mm
 * --------------------------------------------------------------------------- */
int64_t uwb_twr_range_mm(uint64_t t1, uint64_t t2, uint64_t t3,
                          uint64_t t4, uint64_t t5, uint64_t t6)
{
    uint64_t t_round1 = (t4 - t1) & TWR_RESPONDER_TIME_MASK_40BIT;
    uint64_t t_reply1 = (t3 - t2) & TWR_RESPONDER_TIME_MASK_40BIT;
    uint64_t t_round2 = (t6 - t3) & TWR_RESPONDER_TIME_MASK_40BIT;
    uint64_t t_reply2 = (t5 - t4) & TWR_RESPONDER_TIME_MASK_40BIT;
    uint64_t denom = t_round1 + t_round2 + t_reply1 + t_reply2;
    int64_t numerator;
    int64_t tof_ticks;

    if (denom == 0u) {
        return 0;
    }

    numerator = (int64_t)(t_round1 * t_round2) - (int64_t)(t_reply1 * t_reply2);
    tof_ticks = numerator / (int64_t)denom;

    return (int64_t)((double)tof_ticks * TWR_RESPONDER_MM_PER_TICK);
}

/* ---------------------------------------------------------------------------
 * twr_responder_run_once
 * --------------------------------------------------------------------------- */
twr_responder_status_t twr_responder_run_once(uint16_t self_addr,
                                               uwb_twr_exchange_t *out)
{
    uint8_t rx_buf[TWR_RESPONDER_RX_BUF_SIZE];
    uint16_t len;
    uint64_t rx_ts;
    int ret;

    uint16_t initiator_addr;
    uint16_t exchange_id;
    uint64_t t2;
    uint64_t t3;

    uint8_t resp_buf[UWB_TWR_RESPONSE_FRAME_SIZE];
    int resp_len;

    uint16_t final_exchange_id;
    uint64_t t1;
    uint64_t t4;
    uint64_t t5;
    uint64_t t6;

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }

    /* ---- 1. Wait for a POLL addressed to us --------------------------- */
    len = sizeof(rx_buf);
    ret = dw1000_rx(rx_buf, &len, &rx_ts, CONFIG_UWB_TWR_RESPONDER_POLL_TIMEOUT_US);
    if (ret != 0) {
        /* -ETIMEDOUT (nothing heard) or -EIO (RX/FCS error): no POLL this
         * attempt. Not an error condition -- the caller re-arms by calling
         * again. */
        return TWR_RESPONDER_NO_POLL;
    }

    ret = uwb_parse_twr_poll(rx_buf, len, &initiator_addr, &exchange_id);
    if (ret != 0) {
        /* Wrong length / Frame Control / PAN ID / frame type -- not a POLL
         * we understand. */
        LOG_DBG("Heard a non-POLL frame while waiting for POLL (%d)", ret);
        return TWR_RESPONDER_FOREIGN_FRAME;
    }

    /* uwb_parse_twr_poll() does not surface dest_addr -- check it directly
     * against the raw frame to confirm this POLL is unicast to us. */
    if (decode_le16(&rx_buf[UWB_OFF_DEST_ADDR]) != self_addr) {
        LOG_DBG("POLL addressed to 0x%04X, not us (0x%04X) -- ignoring",
                (unsigned)decode_le16(&rx_buf[UWB_OFF_DEST_ADDR]), (unsigned)self_addr);
        return TWR_RESPONDER_FOREIGN_FRAME;
    }

    t2 = rx_ts;

    /* ---- 2. Build + schedule RESPONSE --------------------------------- */
    t3 = dw1000_delayed_tx_time(t2, CONFIG_UWB_TWR_RESP_REPLY_DELAY_DTU);

    resp_len = uwb_build_twr_response(resp_buf, exchange_id, initiator_addr,
                                       self_addr, next_seq(), t2, t3);
    if (resp_len < 0) {
        LOG_ERR("uwb_build_twr_response() failed (%d)", resp_len);
        return TWR_RESPONDER_TX_ERROR;
    }

    ret = dw1000_tx_at(resp_buf, (uint16_t)(resp_len + TWR_RESPONDER_CRC_LEN), t3, true);
    if (ret != 0) {
        LOG_ERR("RESPONSE tx failed for exchange 0x%04X with 0x%04X (%d)",
                exchange_id, initiator_addr, ret);
        return TWR_RESPONDER_TX_ERROR;
    }

    /* ---- 3. Wait for the matching FINAL --------------------------------- */
    len = sizeof(rx_buf);
    ret = dw1000_rx(rx_buf, &len, &rx_ts, CONFIG_UWB_TWR_RESPONDER_FINAL_TIMEOUT_US);
    if (ret != 0) {
        LOG_WRN("No FINAL for exchange 0x%04X with 0x%04X (%d)",
                exchange_id, initiator_addr, ret);
        return TWR_RESPONDER_NO_FINAL;
    }

    ret = uwb_parse_twr_final(rx_buf, len, &final_exchange_id, &t1, &t4, &t5);
    if (ret != 0) {
        LOG_WRN("Frame heard while awaiting FINAL for exchange 0x%04X is not "
                "a valid FINAL (%d)", exchange_id, ret);
        return TWR_RESPONDER_NO_FINAL;
    }

    if (final_exchange_id != exchange_id) {
        LOG_WRN("FINAL exchange_id mismatch: expected 0x%04X, got 0x%04X -- ignoring",
                exchange_id, final_exchange_id);
        return TWR_RESPONDER_FINAL_MISMATCH;
    }

    t6 = rx_ts;

    if (out != NULL) {
        out->initiator_addr = initiator_addr;
        out->exchange_id = exchange_id;
        out->poll_tx_ts = t1;
        out->poll_rx_ts = t2;
        out->resp_tx_ts = t3;
        out->resp_rx_ts = t4;
        out->final_tx_ts = t5;
        out->final_rx_ts = t6;

#if defined(CONFIG_UWB_TWR_RESPONDER_DEBUG_RANGE)
        out->range_mm = uwb_twr_range_mm(t1, t2, t3, t4, t5, t6);
        LOG_INF("DS-TWR exchange 0x%04X with 0x%04X: range = %lld mm",
                exchange_id, initiator_addr, (long long)out->range_mm);
#endif
    }

    return TWR_RESPONDER_EXCHANGE_OK;
}
