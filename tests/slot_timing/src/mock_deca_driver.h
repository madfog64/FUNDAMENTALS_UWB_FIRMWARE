/*! ----------------------------------------------------------------------------
 * @file    mock_deca_driver.h
 * @brief   Mock state for the deca_driver API functions called by
 *          dw1000_ranging.c, linked into the uwb_slot_timing unit test
 *          (UWB-251) purely to satisfy dw1000_ranging.c's link-time symbols.
 *          Identical to tests/dw1000_ranging/src/mock_deca_driver.h.
 *
 * Captured/configurable state is reset by mock_deca_reset() before each test.
 * Tests pre-configure the register-read state (mock_reg_state, injected
 * timestamp/payload bytes) before calling the function under test, then
 * assert on the captured call arguments.
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

/**
 * State captured from mock dwt_setrxtimeout() call.
 */
struct mock_rxtimeout_state {
    int    called;   /**< Non-zero if dwt_setrxtimeout() was called. */
    uint16 time;     /**< The 'time' argument passed (DW1000 RX_FWTO units). */
};

/**
 * State captured from mock dwt_rxenable() call, and its configurable
 * return value.
 */
struct mock_rxenable_state {
    int called;         /**< Non-zero if dwt_rxenable() was called. */
    int mode;            /**< The 'mode' argument passed. */
    int return_value;   /**< Value dwt_rxenable() should return (default DWT_SUCCESS). */
};

/**
 * Call counter for mock dwt_forcetrxoff().
 */
struct mock_forcetrxoff_state {
    int called;
};

/**
 * Configurable register-read values and captured register writes, used for
 * both dw1000_rx()'s and dw1000_tx_at()'s SYS_STATUS/RX_FINFO polling.
 *
 * Set sys_status / rx_finfo BEFORE calling the function under test to drive
 * a specific scenario (good frame / RX error / RX timeout / TXFRS / HPDWARN).
 */
struct mock_reg_state {
    uint32 sys_status;       /**< Value returned for dwt_read32bitreg(SYS_STATUS_ID). */
    uint32 rx_finfo;          /**< Value returned for dwt_read32bitreg(RX_FINFO_ID). */
    uint32 last_write_reg;    /**< Last regFileID written via dwt_write32bit{,offset}reg. */
    uint32 last_write_val;    /**< Last value written. */
    int    write_count;       /**< Number of register writes captured. */
};

/**
 * Captured dwt_readrxdata() call arguments, plus the payload bytes the mock
 * copies into the caller's buffer (set injected_payload before the call).
 */
struct mock_readrxdata_state {
    int    called;
    uint16 length;
    uint16 offset;
    uint8  injected_payload[MOCK_MAX_FRAME_LEN];
};

/**
 * Injected 5-byte RX/TX timestamp buffers returned by
 * dwt_readrxtimestamp() / dwt_readtxtimestamp(), plus call counters.
 */
struct mock_timestamp_state {
    uint8 rx_ts_bytes[5];
    uint8 tx_ts_bytes[5];
    int   rx_called;
    int   tx_called;
};

/**
 * State captured from mock dwt_setdelayedtrxtime() call.
 */
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

extern struct mock_rxtimeout_state       mock_rxtimeout_state;
extern struct mock_rxenable_state        mock_rxenable_state;
extern struct mock_forcetrxoff_state     mock_forcetrxoff_state;
extern struct mock_reg_state             mock_reg_state;
extern struct mock_readrxdata_state      mock_readrxdata_state;
extern struct mock_timestamp_state       mock_timestamp_state;
extern struct mock_delayedtrxtime_state  mock_delayedtrxtime_state;
extern struct mock_tx_state              mock_tx_state;
extern struct mock_starttx_state         mock_starttx_state;

/** Reset all captured/configurable state to defaults (all-success, all-zero). */
void mock_deca_reset(void);

#endif /* MOCK_DECA_DRIVER_H_ */
