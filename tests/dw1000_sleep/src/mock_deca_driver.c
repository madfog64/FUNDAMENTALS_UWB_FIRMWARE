/*! ----------------------------------------------------------------------------
 * @file    mock_deca_driver.c
 * @brief   Mock implementations of deca_driver API functions called by
 *          dw1000_sleep()/dw1000_wake() and the dw1000_configure()
 *          reconfigure-on-wake path (UWB-316).
 *
 * These stubs replace deca_device.c and deca_params_init.c in the
 * unit_testing build. They have no hardware or SPI dependencies — they
 * capture arguments (and a call-order sequence number, mock_seq.h) so the
 * test assertions can verify both the sleep/wake call sequence and the
 * dw1000_configure() reconfigure path it drives.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <string.h>
#include "deca_device_api.h"
#include "dw1000_config.h"
#include "mock_deca_driver.h"
#include "mock_seq.h"

/* ---------------------------------------------------------------------------
 * Captured call state (inspected by test assertions)
 * --------------------------------------------------------------------------- */
struct mock_dwt_init_state    mock_init_state;
struct mock_dwt_cfg_state     mock_cfg_state;
struct mock_dwt_txrf_state    mock_txrf_state;
struct mock_dwt_antenna_state mock_antenna_state;
struct mock_dwt_otp_state     mock_otp_state;
struct mock_dwt_sleep_state   mock_sleep_state;

void mock_deca_reset(void)
{
    memset(&mock_init_state,    0, sizeof(mock_init_state));
    memset(&mock_cfg_state,     0, sizeof(mock_cfg_state));
    memset(&mock_txrf_state,    0, sizeof(mock_txrf_state));
    memset(&mock_antenna_state, 0, sizeof(mock_antenna_state));
    memset(&mock_otp_state,     0, sizeof(mock_otp_state));
    memset(&mock_sleep_state,   0, sizeof(mock_sleep_state));
    /* Default: dwt_initialise() returns success; OTP is unprogrammed (0). */
    mock_init_state.return_value = DWT_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_initialise — the "device-id re-read" step of the reconfigure path.
 * --------------------------------------------------------------------------- */
int dwt_initialise(uint16 config)
{
    mock_init_state.called     = 1;
    mock_init_state.config_arg = config;
    mock_init_state.seq        = mock_seq_next();
    return mock_init_state.return_value;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_configure
 * --------------------------------------------------------------------------- */
void dwt_configure(dwt_config_t *config)
{
    mock_cfg_state.called = 1;
    mock_cfg_state.seq    = mock_seq_next();
    if (config != NULL) {
        memcpy(&mock_cfg_state.config, config, sizeof(dwt_config_t));
    }
}

/* ---------------------------------------------------------------------------
 * Mock dwt_configuretxrf
 * --------------------------------------------------------------------------- */
void dwt_configuretxrf(dwt_txconfig_t *config)
{
    mock_txrf_state.called = 1;
    mock_txrf_state.seq    = mock_seq_next();
    if (config != NULL) {
        memcpy(&mock_txrf_state.config, config, sizeof(dwt_txconfig_t));
    }
}

/* ---------------------------------------------------------------------------
 * Mock dwt_settxantennadelay / dwt_setrxantennadelay
 * --------------------------------------------------------------------------- */
void dwt_settxantennadelay(uint16 txDelay)
{
    mock_antenna_state.tx_called++;
    mock_antenna_state.tx_delay = txDelay;
}

void dwt_setrxantennadelay(uint16 rxDelay)
{
    mock_antenna_state.rx_called++;
    mock_antenna_state.rx_delay = rxDelay;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_otpread
 *
 * Returns mock_otp_state.antdly_word for DW1000_OTP_ANTDLY_ADDRESS; 0
 * otherwise. Default (unprogrammed OTP) keeps dw1000_configure()'s antenna
 * delay path on the Kconfig-default branch, which this suite does not assert
 * on (already covered by tests/dw1000_config/).
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
 * Mock dwt_configuresleep / dwt_entersleep (UWB-316)
 * --------------------------------------------------------------------------- */
void dwt_configuresleep(uint16 mode, uint8 wake)
{
    mock_sleep_state.configuresleep_called = 1;
    mock_sleep_state.configuresleep_mode   = mode;
    mock_sleep_state.configuresleep_wake   = wake;
    mock_sleep_state.configuresleep_seq    = mock_seq_next();
}

void dwt_entersleep(void)
{
    mock_sleep_state.entersleep_called = 1;
    mock_sleep_state.entersleep_seq    = mock_seq_next();
}

/* ---------------------------------------------------------------------------
 * Additional deca_driver stubs required at link time.
 * These functions are not exercised by this suite's assertions but may be
 * pulled in by deca_device_api.h declarations or the linker.
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
