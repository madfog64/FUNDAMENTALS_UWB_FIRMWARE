/*! ----------------------------------------------------------------------------
 * @file    mock_deca_driver.h
 * @brief   Mock state for the deca_driver API functions called by
 *          dw1000_configure() (UWB-155, UWB-156 unit tests).
 *
 * Captured state is reset by mock_deca_reset() before each test.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef MOCK_DECA_DRIVER_H_
#define MOCK_DECA_DRIVER_H_

#include "deca_device_api.h"

/**
 * State captured from mock dwt_initialise() call.
 */
struct mock_dwt_init_state {
    int    called;          /**< Non-zero if dwt_initialise() was called. */
    uint16 config_arg;      /**< The 'config' argument (e.g. DWT_LOADUCODE). */
    int    return_value;    /**< Value that dwt_initialise() should return. */
};

/**
 * State captured from mock dwt_configure() call.
 */
struct mock_dwt_cfg_state {
    int         called;     /**< Non-zero if dwt_configure() was called. */
    dwt_config_t config;    /**< Copy of the dwt_config_t passed to dwt_configure(). */
};

/**
 * State captured from mock dwt_configuretxrf() call.
 */
struct mock_dwt_txrf_state {
    int            called;  /**< Non-zero if dwt_configuretxrf() was called. */
    dwt_txconfig_t config;  /**< Copy of the dwt_txconfig_t passed to dwt_configuretxrf(). */
};

/**
 * State captured from mock dwt_settxantennadelay() and
 * dwt_setrxantennadelay() calls (UWB-156).
 */
struct mock_dwt_antenna_state {
    int    tx_called;       /**< Number of dwt_settxantennadelay() calls. */
    uint16 tx_delay;        /**< Last value passed to dwt_settxantennadelay(). */
    int    rx_called;       /**< Number of dwt_setrxantennadelay() calls. */
    uint16 rx_delay;        /**< Last value passed to dwt_setrxantennadelay(). */
};

/**
 * Configurable OTP state for mock dwt_otpread() (UWB-156).
 *
 * antdly_word is the 32-bit value returned when dwt_otpread() is called with
 * address DW1000_OTP_ANTDLY_ADDRESS (0x1C).  Set to 0 (default) to simulate
 * an unprogrammed OTP; set bits[31:16] to a non-zero PRF-64 delay to simulate
 * a factory-calibrated device.
 */
struct mock_dwt_otp_state {
    uint32 antdly_word;     /**< OTP word returned for address 0x1C. */
};

extern struct mock_dwt_init_state    mock_init_state;
extern struct mock_dwt_cfg_state     mock_cfg_state;
extern struct mock_dwt_txrf_state    mock_txrf_state;
extern struct mock_dwt_antenna_state mock_antenna_state;
extern struct mock_dwt_otp_state     mock_otp_state;

/** Reset all captured state and set dwt_initialise return value to DWT_SUCCESS. */
void mock_deca_reset(void);

#endif /* MOCK_DECA_DRIVER_H_ */
