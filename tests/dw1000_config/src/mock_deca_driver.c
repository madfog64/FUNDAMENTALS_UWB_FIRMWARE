/*! ----------------------------------------------------------------------------
 * @file    mock_deca_driver.c
 * @brief   Mock implementations of deca_driver API functions called by
 *          dw1000_configure() (UWB-155, UWB-156 unit tests).
 *
 * These stubs replace deca_device.c and deca_params_init.c in the
 * unit_testing build.  They have no hardware or SPI dependencies — they
 * capture arguments so the test assertions can verify dw1000_configure()
 * drove the driver with the correct parameters.
 *
 * UWB-156 additions:
 *   dwt_settxantennadelay()  — captures last TX delay value and call count
 *   dwt_setrxantennadelay()  — captures last RX delay value and call count
 *   dwt_otpread()            — returns mock_otp_state.antdly_word for address
 *                              DW1000_OTP_ANTDLY_ADDRESS (0x1C); zero otherwise
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <string.h>
#include "deca_device_api.h"
#include "dw1000_config.h"
#include "mock_deca_driver.h"

/* ---------------------------------------------------------------------------
 * Captured call state (inspected by test assertions)
 * --------------------------------------------------------------------------- */
struct mock_dwt_init_state    mock_init_state;
struct mock_dwt_cfg_state     mock_cfg_state;
struct mock_dwt_txrf_state    mock_txrf_state;
struct mock_dwt_antenna_state mock_antenna_state;
struct mock_dwt_otp_state     mock_otp_state;

void mock_deca_reset(void)
{
    memset(&mock_init_state,    0, sizeof(mock_init_state));
    memset(&mock_cfg_state,     0, sizeof(mock_cfg_state));
    memset(&mock_txrf_state,    0, sizeof(mock_txrf_state));
    memset(&mock_antenna_state, 0, sizeof(mock_antenna_state));
    memset(&mock_otp_state,     0, sizeof(mock_otp_state));
    /* Default: dwt_initialise() returns success; OTP is unprogrammed (0). */
    mock_init_state.return_value = DWT_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_initialise
 *
 * Captures the 'config' argument (DWT_LOADUCODE vs DWT_LOADNONE) and returns
 * the pre-set return_value (default: DWT_SUCCESS).
 * --------------------------------------------------------------------------- */
int dwt_initialise(uint16 config)
{
    mock_init_state.called     = 1;
    mock_init_state.config_arg = config;
    return mock_init_state.return_value;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_configure
 *
 * Captures a copy of the dwt_config_t struct so the test can assert each
 * field value independently.
 * --------------------------------------------------------------------------- */
void dwt_configure(dwt_config_t *config)
{
    mock_cfg_state.called = 1;
    if (config != NULL) {
        memcpy(&mock_cfg_state.config, config, sizeof(dwt_config_t));
    }
}

/* ---------------------------------------------------------------------------
 * Mock dwt_configuretxrf
 *
 * Captures a copy of the dwt_txconfig_t struct.
 * --------------------------------------------------------------------------- */
void dwt_configuretxrf(dwt_txconfig_t *config)
{
    mock_txrf_state.called = 1;
    if (config != NULL) {
        memcpy(&mock_txrf_state.config, config, sizeof(dwt_txconfig_t));
    }
}

/* ---------------------------------------------------------------------------
 * Mock dwt_settxantennadelay (UWB-156)
 *
 * Captures the TX delay value and increments the call counter.
 * --------------------------------------------------------------------------- */
void dwt_settxantennadelay(uint16 txDelay)
{
    mock_antenna_state.tx_called++;
    mock_antenna_state.tx_delay = txDelay;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_setrxantennadelay (UWB-156)
 *
 * Captures the RX delay value and increments the call counter.
 * --------------------------------------------------------------------------- */
void dwt_setrxantennadelay(uint16 rxDelay)
{
    mock_antenna_state.rx_called++;
    mock_antenna_state.rx_delay = rxDelay;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_otpread (UWB-156)
 *
 * Returns mock_otp_state.antdly_word when the requested address matches
 * DW1000_OTP_ANTDLY_ADDRESS (0x1C).  All other addresses return 0.
 *
 * Set mock_otp_state.antdly_word before calling dw1000_configure() to
 * simulate a factory-calibrated (non-zero) or unprogrammed (zero) OTP:
 *   - Unprogrammed: antdly_word = 0           (default after mock_deca_reset)
 *   - PRF-64 cal'd: antdly_word = (val << 16) (val in bits[31:16])
 * --------------------------------------------------------------------------- */
void dwt_otpread(uint32 address, uint32 *array, uint8 length)
{
    uint8 i;

    if (array == NULL || length == 0u) {
        return;
    }
    for (i = 0u; i < length; i++) {
        if (address + i == (uint32)DW1000_OTP_ANTDLY_ADDRESS) {
            array[i] = mock_otp_state.antdly_word;
        } else {
            array[i] = 0u;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Additional deca_driver stubs required at link time.
 * These functions are not called by dw1000_configure() but may be pulled in
 * by deca_device_api.h declarations or the linker.
 * --------------------------------------------------------------------------- */

uint32 dwt_readdevid(void)         { return DWT_DEVICE_ID; }
uint32 dwt_getpartid(void)         { return 0; }
uint32 dwt_getlotid(void)          { return 0; }
uint8  dwt_otprevision(void)       { return 0; }
int    dwt_setlocaldataptr(unsigned int index) { (void)index; return DWT_SUCCESS; }
uint32 dwt_read32bitoffsetreg(int regFileID, int regOffset) {
    (void)regFileID; (void)regOffset; return 0;
}
void   dwt_write32bitoffsetreg(int regFileID, int regOffset, uint32 regval) {
    (void)regFileID; (void)regOffset; (void)regval;
}
uint16 dwt_read16bitoffsetreg(int regFileID, int regOffset) {
    (void)regFileID; (void)regOffset; return 0;
}
void   dwt_write16bitoffsetreg(int regFileID, int regOffset, uint16 regval) {
    (void)regFileID; (void)regOffset; (void)regval;
}
uint8  dwt_read8bitoffsetreg(int regFileID, int regOffset) {
    (void)regFileID; (void)regOffset; return 0;
}
void   dwt_write8bitoffsetreg(int regFileID, int regOffset, uint8 regval) {
    (void)regFileID; (void)regOffset; (void)regval;
}
void   dwt_readfromdevice(uint16 r, uint16 i, uint32 l, uint8 *b) {
    (void)r; (void)i; (void)l; (void)b;
}
void   dwt_writetodevice(uint16 r, uint16 i, uint32 l, const uint8 *b) {
    (void)r; (void)i; (void)l; (void)b;
}
