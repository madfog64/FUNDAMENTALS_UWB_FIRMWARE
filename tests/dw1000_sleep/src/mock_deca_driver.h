/*! ----------------------------------------------------------------------------
 * @file    mock_deca_driver.h
 * @brief   Mock state for the deca_driver API functions called by
 *          dw1000_sleep()/dw1000_wake() and the dw1000_configure()
 *          reconfigure-on-wake path they drive (UWB-316).
 *
 * Captured state is reset by mock_deca_reset() before each test. Each struct
 * carries a `seq` field (see mock_seq.h) stamped at call time, so tests can
 * assert call ORDER, not just call presence/count.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef MOCK_DECA_DRIVER_H_
#define MOCK_DECA_DRIVER_H_

#include "deca_device_api.h"

/** State captured from mock dwt_initialise() — the "device-id re-read". */
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

extern struct mock_dwt_init_state    mock_init_state;
extern struct mock_dwt_cfg_state     mock_cfg_state;
extern struct mock_dwt_txrf_state    mock_txrf_state;
extern struct mock_dwt_antenna_state mock_antenna_state;
extern struct mock_dwt_otp_state     mock_otp_state;
extern struct mock_dwt_sleep_state   mock_sleep_state;

/** Reset all captured state; dwt_initialise() defaults to returning DWT_SUCCESS. */
void mock_deca_reset(void);

#endif /* MOCK_DECA_DRIVER_H_ */
