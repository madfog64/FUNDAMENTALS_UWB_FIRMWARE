/*! ----------------------------------------------------------------------------
 * @file    mock_deca_driver.h
 * @brief   Mock state for the deca_driver API functions called by
 *          dw1000_ranging.c (UWB-231) and exercised transitively by
 *          twr_responder.c (UWB-232 unit tests).
 *
 * Adapted from tests/dw1000_ranging/src/mock_deca_driver.{c,h} (same
 * function-level mocking approach, no hardware/SPI dependency) with one
 * addition: an RX *queue* (mock_rx_queue_push()), because a single
 * twr_responder_run_once() attempt can call dw1000_rx() up to twice (once
 * waiting for a POLL, once waiting for the matching FINAL) and each call
 * needs to be able to report a different frame/timestamp/status. TX-side
 * mocking (dw1000_tx_at() is called at most once per attempt) is unchanged
 * from the UWB-231 mock: single captured-call state, no queue needed.
 *
 * Reset by mock_deca_reset() before each test. Tests push the RX events they
 * want dw1000_rx() to observe (in call order) via mock_rx_queue_push() and/or
 * configure TX-side return values, then assert on the captured call
 * arguments.
 *
 * Note: dw1000_ranging.c's dw1000_rx() and dw1000_tx_at() both poll the same
 * hardware register (SYS_STATUS_ID) for different bits (RXFCG/timeout/error
 * vs TXFRS). The mock therefore models a single "live" SYS_STATUS value
 * internally (loaded from the active RX event on dwt_rxenable(), OR'd with
 * TXFRS by a successful dwt_starttx(), and write-1-to-clear on
 * dwt_write32bitoffsetreg()) -- see mock_deca_driver.c. Tests only interact
 * with this indirectly, via mock_rx_queue_push() and mock_starttx_state.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef MOCK_DECA_DRIVER_H_
#define MOCK_DECA_DRIVER_H_

#include "deca_device_api.h"

/** Size of the scratch buffers used to capture RX/TX payload bytes. */
#define MOCK_MAX_FRAME_LEN  128

/** Max number of RX events a single test can program. A DS-TWR responder
 *  attempt calls dw1000_rx() at most twice (POLL wait, FINAL wait); a small
 *  margin is kept for tests that call twr_responder_run_once() more than
 *  once in a row. */
#define MOCK_RX_QUEUE_DEPTH  6

/**
 * One programmed dw1000_rx() outcome: what SYS_STATUS / RX_FINFO /
 * dwt_readrxdata() / dwt_readrxtimestamp() should report for one call to
 * dw1000_rx().
 */
struct mock_rx_event {
    uint32 sys_status;                    /**< e.g. SYS_STATUS_RXFCG / *_RXRFTO / *_RXFCE. */
    uint32 rx_finfo;                       /**< Reported frame length (RX_FINFO_RXFLEN_MASK bits). */
    uint8  payload[MOCK_MAX_FRAME_LEN];    /**< Frame bytes dwt_readrxdata() copies out. */
    uint16 payload_len;                    /**< Valid bytes in payload[] (informational). */
    uint8  rx_ts_bytes[5];                 /**< 40-bit RX timestamp, little-endian. */
};

/**
 * Queue of programmed RX events, served strictly in order: each dwt_rxenable()
 * call (exactly one per dw1000_rx() call) advances to the next queued event.
 * If dw1000_rx() is called more times than were programmed, unprogrammed
 * calls report a frame-wait timeout (SYS_STATUS_RXRFTO) -- see
 * mock_deca_reset()/dwt_rxenable() in mock_deca_driver.c.
 */
struct mock_rx_queue_state {
    struct mock_rx_event events[MOCK_RX_QUEUE_DEPTH];
    int count;        /**< Number of events pushed via mock_rx_queue_push(). */
    int next_index;   /**< Index of the next event dwt_rxenable() will serve. */
};

/**
 * State captured from mock dwt_setrxtimeout() call. .called counts every
 * invocation (a run_once() attempt may call dw1000_rx() -> dwt_setrxtimeout()
 * up to twice); .time holds the most recent 'time' argument.
 */
struct mock_rxtimeout_state {
    int    called;
    uint16 time;
};

/**
 * State captured from mock dwt_rxenable() calls, and its configurable return
 * value. .called counts every invocation -- also used as the RX-queue
 * advance trigger (see mock_rx_queue_state).
 */
struct mock_rxenable_state {
    int called;
    int mode;           /**< Most recent 'mode' argument passed. */
    int return_value;   /**< Value dwt_rxenable() should return (default DWT_SUCCESS). */
};

/** Call counter for mock dwt_forcetrxoff(). */
struct mock_forcetrxoff_state {
    int called;
};

/**
 * Captured register-write state (SYS_STATUS / TXFRS clears etc.) -- write
 * side only. Read side (SYS_STATUS_ID / RX_FINFO_ID) is served from the
 * active mock_rx_event during dw1000_rx()'s polling loop instead; see
 * mock_deca_driver.c.
 */
struct mock_reg_state {
    uint32 last_write_reg;
    uint32 last_write_val;
    int    write_count;
};

/** Captured dwt_readrxdata() call arguments (payload bytes themselves come
 *  from the active mock_rx_event, not stored here). */
struct mock_readrxdata_state {
    int    called;
    uint16 length;
    uint16 offset;
};

/**
 * Captured dwt_readrxtimestamp() / dwt_readtxtimestamp() call counts. RX
 * timestamp bytes come from the active mock_rx_event; TX timestamp bytes are
 * a single injectable buffer (dw1000_tx_at() does not call
 * dwt_readtxtimestamp() itself -- kept for API parity with the UWB-231 mock
 * / dw1000_ranging.c's dw1000_read_tx_timestamp(), unused by this suite).
 */
struct mock_timestamp_state {
    uint8 tx_ts_bytes[5];
    int   rx_called;
    int   tx_called;
};

/** State captured from mock dwt_setdelayedtrxtime() call. */
struct mock_delayedtrxtime_state {
    int    called;
    uint32 starttime;
};

/**
 * State captured from mock dwt_writetxdata() and dwt_writetxfctrl() calls,
 * and dwt_writetxdata()'s configurable return value.
 */
struct mock_tx_state {
    int    writetxdata_called;
    uint16 writetxdata_len;
    uint8  writetxdata_buf[MOCK_MAX_FRAME_LEN];
    uint16 writetxdata_offset;
    int    writetxdata_return_value;

    int    writetxfctrl_called;
    uint16 writetxfctrl_len;
    uint16 writetxfctrl_offset;
    int    writetxfctrl_ranging;
};

/**
 * State captured from mock dwt_starttx() call, and its configurable return
 * value (set to DWT_ERROR to simulate a missed/HPDWARN slot).
 */
struct mock_starttx_state {
    int    called;
    uint8  mode;
    int    return_value;
};

extern struct mock_rx_queue_state        mock_rx_queue_state;
extern struct mock_rxtimeout_state       mock_rxtimeout_state;
extern struct mock_rxenable_state        mock_rxenable_state;
extern struct mock_forcetrxoff_state     mock_forcetrxoff_state;
extern struct mock_reg_state             mock_reg_state;
extern struct mock_readrxdata_state      mock_readrxdata_state;
extern struct mock_timestamp_state       mock_timestamp_state;
extern struct mock_delayedtrxtime_state  mock_delayedtrxtime_state;
extern struct mock_tx_state              mock_tx_state;
extern struct mock_starttx_state         mock_starttx_state;

/** Reset all captured/configurable state to defaults (all-success, all-zero,
 *  empty RX queue). */
void mock_deca_reset(void);

/**
 * @brief Program the outcome of the next (not-yet-served) dw1000_rx() call.
 *
 * Push events in the order the code under test is expected to call
 * dw1000_rx() -- e.g. for a full DS-TWR exchange: push the POLL event first,
 * then the FINAL event.
 *
 * @param sys_status    SYS_STATUS bits dw1000_rx()'s polling loop should see
 *                       (e.g. SYS_STATUS_RXFCG for a good frame,
 *                       SYS_STATUS_RXRFTO for a timeout, SYS_STATUS_RXFCE for
 *                       an RX/FCS error).
 * @param rx_finfo       Value RX_FINFO should report (frame length in the
 *                       RX_FINFO_RXFLEN_MASK bits). Ignored for
 *                       timeout/error events.
 * @param payload        Frame bytes dwt_readrxdata() should copy out (may be
 *                       NULL for timeout/error events).
 * @param payload_len    Number of valid bytes in @p payload (also informs
 *                       the copy length; capped to MOCK_MAX_FRAME_LEN).
 * @param rx_ts_bytes    5-byte little-endian RX timestamp dwt_readrxtimestamp()
 *                       should report (may be NULL -- treated as all-zero).
 */
void mock_rx_queue_push(uint32 sys_status, uint32 rx_finfo,
                         const uint8 *payload, uint16 payload_len,
                         const uint8 rx_ts_bytes[5]);

#endif /* MOCK_DECA_DRIVER_H_ */
