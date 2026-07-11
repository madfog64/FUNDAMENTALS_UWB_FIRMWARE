/*! ----------------------------------------------------------------------------
 * @file    mock_deca_driver.h
 * @brief   Mock state for the deca_driver API surface exercised by
 *          uwb_standby.c's composed dependencies (UWB-319 ztest suite):
 *          dw1000_sleep.c (dwt_configuresleep/entersleep), dw1000_config.c
 *          (dw1000_wake()'s reconfigure-on-wake path -- dwt_initialise/
 *          configure/configuretxrf/settxantennadelay/setrxantennadelay/
 *          otpread), and dw1000_ranging.c + dw1000_sync.c (the
 *          presence-check dw1000_sync_rx() -> dw1000_rx() call).
 *
 * This is the union of tests/dw1000_sleep/src/mock_deca_driver.h (UWB-316)
 * and tests/join/src/mock_deca_driver.h (UWB-261) -- uwb_standby.c is the
 * first module to compose both seams in the same call path (ASLEEP: wake
 * -- the UWB-316 reconfigure path -- immediately followed by one
 * dw1000_sync_rx() presence-check listen), so its test build needs both
 * mock surfaces linked together. Only dwt_read32bitoffsetreg() /
 * dwt_write32bitoffsetreg() overlap between the two sources; this merge
 * keeps the tests/join richer register-state version (SYS_STATUS_ID /
 * RX_FINFO_ID awareness, needed for the RX poll loop in dw1000_rx()) rather
 * than tests/dw1000_sleep's always-returns-0 stub.
 *
 * Captured/configurable state is reset by mock_deca_reset() before each
 * test. Each sleep/wake-side struct carries a `seq` field (see mock_seq.h)
 * stamped at call time so tests can assert call ORDER, not just call
 * presence/count, mirroring tests/dw1000_sleep's own convention.
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

/* ---------------------------------------------------------------------------
 * dw1000_config.c / dw1000_sleep.c side (reconfigure-on-wake path)
 * --------------------------------------------------------------------------- */

/** State captured from mock dwt_initialise() -- the "device-id re-read". */
struct mock_dwt_init_state {
    int    called;
    uint16 config_arg;
    int    return_value;
    int    seq;
};

/** State captured from mock dwt_configure(). */
struct mock_dwt_cfg_state {
    int          called;
    dwt_config_t config;
    int          seq;
};

/** State captured from mock dwt_configuretxrf(). */
struct mock_dwt_txrf_state {
    int            called;
    dwt_txconfig_t config;
    int            seq;
};

/** State captured from mock dwt_settxantennadelay() / dwt_setrxantennadelay(). */
struct mock_dwt_antenna_state {
    int    tx_called;
    uint16 tx_delay;
    int    rx_called;
    uint16 rx_delay;
};

/** Configurable OTP state for mock dwt_otpread(). */
struct mock_dwt_otp_state {
    uint32 antdly_word;
};

/** State captured from mock dwt_configuresleep() / dwt_entersleep() (UWB-316). */
struct mock_dwt_sleep_state {
    int    configuresleep_called;
    uint16 configuresleep_mode;
    uint8  configuresleep_wake;
    int    configuresleep_seq;

    int    entersleep_called;
    int    entersleep_seq;
};

/* ---------------------------------------------------------------------------
 * dw1000_ranging.c side (dw1000_rx(), via dw1000_sync_rx() presence check)
 * --------------------------------------------------------------------------- */

/** State captured from mock dwt_setrxtimeout() call. */
struct mock_rxtimeout_state {
    int    called;
    uint16 time;
};

/** State captured from mock dwt_rxenable() call, and its configurable
 *  return value. */
struct mock_rxenable_state {
    int called;
    int mode;
    int return_value;
};

/** Call counter for mock dwt_forcetrxoff(). */
struct mock_forcetrxoff_state {
    int called;
};

/**
 * Configurable register-read values and captured register writes, used for
 * dw1000_rx()'s SYS_STATUS/RX_FINFO polling.
 *
 * Set sys_status / rx_finfo BEFORE calling the function under test to drive
 * a specific scenario (good frame / RX error / RX timeout).
 */
struct mock_reg_state {
    uint32 sys_status;
    uint32 rx_finfo;
    uint32 last_write_reg;
    uint32 last_write_val;
    int    write_count;
};

/** Captured dwt_readrxdata() call arguments, plus the payload bytes the
 *  mock copies into the caller's buffer (set injected_payload before the
 *  call). */
struct mock_readrxdata_state {
    int    called;
    uint16 length;
    uint16 offset;
    uint8  injected_payload[MOCK_MAX_FRAME_LEN];
};

/** Injected 5-byte RX/TX timestamp buffers returned by
 *  dwt_readrxtimestamp() / dwt_readtxtimestamp(), plus call counters. */
struct mock_timestamp_state {
    uint8 rx_ts_bytes[5];
    uint8 tx_ts_bytes[5];
    int   rx_called;
    int   tx_called;
};

/** State captured from mock dwt_setdelayedtrxtime() call (link-time only --
 *  uwb_standby.c never TXes, so dw1000_tx_at()'s half of dw1000_ranging.c
 *  is never exercised in this suite). */
struct mock_delayedtrxtime_state {
    int    called;
    uint32 starttime;
};

/** State captured from mock dwt_writetxdata() and dwt_writetxfctrl() calls
 *  (link-time only -- see mock_delayedtrxtime_state). */
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

/** State captured from mock dwt_starttx() call (link-time only -- see
 *  mock_delayedtrxtime_state). */
struct mock_starttx_state {
    int    called;
    uint8  mode;
    int    return_value;
};

extern struct mock_dwt_init_state       mock_init_state;
extern struct mock_dwt_cfg_state        mock_cfg_state;
extern struct mock_dwt_txrf_state       mock_txrf_state;
extern struct mock_dwt_antenna_state    mock_antenna_state;
extern struct mock_dwt_otp_state        mock_otp_state;
extern struct mock_dwt_sleep_state      mock_sleep_state;

extern struct mock_rxtimeout_state      mock_rxtimeout_state;
extern struct mock_rxenable_state       mock_rxenable_state;
extern struct mock_forcetrxoff_state    mock_forcetrxoff_state;
extern struct mock_reg_state            mock_reg_state;
extern struct mock_readrxdata_state     mock_readrxdata_state;
extern struct mock_timestamp_state      mock_timestamp_state;
extern struct mock_delayedtrxtime_state mock_delayedtrxtime_state;
extern struct mock_tx_state             mock_tx_state;
extern struct mock_starttx_state        mock_starttx_state;

/** Reset all captured/configurable state to defaults (all-success, all-zero). */
void mock_deca_reset(void);

#endif /* MOCK_DECA_DRIVER_H_ */
