/*! ----------------------------------------------------------------------------
 * @file    mock_deca_driver.c
 * @brief   Mock implementations of the deca_driver API surface exercised by
 *          uwb_standby.c's composed dependencies (UWB-319). See
 *          mock_deca_driver.h for provenance -- this is the union of
 *          tests/dw1000_sleep/src/mock_deca_driver.c (UWB-316) and
 *          tests/join/src/mock_deca_driver.c (UWB-261).
 *
 * These stubs replace deca_device.c and deca_params_init.c in the
 * unit_testing build. They have no hardware or SPI dependencies.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <string.h>

#include "deca_device_api.h"
#include "deca_regs.h"
#include "dw1000_config.h"
#include "mock_deca_driver.h"
#include "mock_seq.h"

/* ---------------------------------------------------------------------------
 * Captured / configurable state (inspected and pre-configured by tests)
 * --------------------------------------------------------------------------- */
struct mock_dwt_init_state       mock_init_state;
struct mock_dwt_cfg_state        mock_cfg_state;
struct mock_dwt_txrf_state       mock_txrf_state;
struct mock_dwt_antenna_state    mock_antenna_state;
struct mock_dwt_otp_state        mock_otp_state;
struct mock_dwt_sleep_state      mock_sleep_state;

struct mock_rxtimeout_state      mock_rxtimeout_state;
struct mock_rxenable_state       mock_rxenable_state;
struct mock_forcetrxoff_state    mock_forcetrxoff_state;
struct mock_reg_state            mock_reg_state;
struct mock_readrxdata_state     mock_readrxdata_state;
struct mock_timestamp_state      mock_timestamp_state;
struct mock_delayedtrxtime_state mock_delayedtrxtime_state;
struct mock_tx_state             mock_tx_state;
struct mock_starttx_state        mock_starttx_state;

void mock_deca_reset(void)
{
    memset(&mock_init_state,    0, sizeof(mock_init_state));
    memset(&mock_cfg_state,     0, sizeof(mock_cfg_state));
    memset(&mock_txrf_state,    0, sizeof(mock_txrf_state));
    memset(&mock_antenna_state, 0, sizeof(mock_antenna_state));
    memset(&mock_otp_state,     0, sizeof(mock_otp_state));
    memset(&mock_sleep_state,   0, sizeof(mock_sleep_state));

    memset(&mock_rxtimeout_state,      0, sizeof(mock_rxtimeout_state));
    memset(&mock_rxenable_state,       0, sizeof(mock_rxenable_state));
    memset(&mock_forcetrxoff_state,    0, sizeof(mock_forcetrxoff_state));
    memset(&mock_reg_state,            0, sizeof(mock_reg_state));
    memset(&mock_readrxdata_state,     0, sizeof(mock_readrxdata_state));
    memset(&mock_timestamp_state,      0, sizeof(mock_timestamp_state));
    memset(&mock_delayedtrxtime_state, 0, sizeof(mock_delayedtrxtime_state));
    memset(&mock_tx_state,              0, sizeof(mock_tx_state));
    memset(&mock_starttx_state,         0, sizeof(mock_starttx_state));

    /* Defaults: every deca_driver call that can fail reports success; OTP is
     * unprogrammed (0). */
    mock_init_state.return_value           = DWT_SUCCESS;
    mock_rxenable_state.return_value        = DWT_SUCCESS;
    mock_tx_state.writetxdata_return_value  = DWT_SUCCESS;
    mock_starttx_state.return_value         = DWT_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_initialise -- the "device-id re-read" step of the reconfigure path.
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
 * Mock dwt_setrxtimeout
 * --------------------------------------------------------------------------- */
void dwt_setrxtimeout(uint16 time)
{
    mock_rxtimeout_state.called = 1;
    mock_rxtimeout_state.time   = time;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_rxenable
 * --------------------------------------------------------------------------- */
int dwt_rxenable(int mode)
{
    mock_rxenable_state.called = 1;
    mock_rxenable_state.mode   = mode;
    return mock_rxenable_state.return_value;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_forcetrxoff
 * --------------------------------------------------------------------------- */
void dwt_forcetrxoff(void)
{
    mock_forcetrxoff_state.called++;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_read32bitoffsetreg (backs the dwt_read32bitreg() macro)
 *
 * Returns mock_reg_state.sys_status for SYS_STATUS_ID, mock_reg_state.rx_finfo
 * for RX_FINFO_ID, and 0 for anything else.
 * --------------------------------------------------------------------------- */
uint32 dwt_read32bitoffsetreg(int regFileID, int regOffset)
{
    (void)regOffset;

    if (regFileID == SYS_STATUS_ID) {
        return mock_reg_state.sys_status;
    }
    if (regFileID == RX_FINFO_ID) {
        return mock_reg_state.rx_finfo;
    }
    return 0u;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_write32bitoffsetreg (backs the dwt_write32bitreg() macro)
 *
 * Captures the last register/value written. When writing SYS_STATUS_ID,
 * clears the written bits from mock_reg_state.sys_status (write-1-to-clear,
 * matching real DW1000 SYS_STATUS semantics).
 * --------------------------------------------------------------------------- */
void dwt_write32bitoffsetreg(int regFileID, int regOffset, uint32 regval)
{
    (void)regOffset;

    mock_reg_state.last_write_reg = (uint32)regFileID;
    mock_reg_state.last_write_val = regval;
    mock_reg_state.write_count++;

    if (regFileID == SYS_STATUS_ID) {
        mock_reg_state.sys_status &= ~regval;
    }
}

/* ---------------------------------------------------------------------------
 * Mock dwt_readrxdata
 * --------------------------------------------------------------------------- */
void dwt_readrxdata(uint8 *buffer, uint16 length, uint16 rxBufferOffset)
{
    uint16 copy_len = length;

    mock_readrxdata_state.called = 1;
    mock_readrxdata_state.length = length;
    mock_readrxdata_state.offset = rxBufferOffset;

    if (copy_len > MOCK_MAX_FRAME_LEN) {
        copy_len = MOCK_MAX_FRAME_LEN;
    }
    if (buffer != NULL) {
        memcpy(buffer, mock_readrxdata_state.injected_payload, copy_len);
    }
}

/* ---------------------------------------------------------------------------
 * Mock dwt_readrxtimestamp / dwt_readtxtimestamp
 * --------------------------------------------------------------------------- */
void dwt_readrxtimestamp(uint8 *timestamp)
{
    mock_timestamp_state.rx_called++;
    if (timestamp != NULL) {
        memcpy(timestamp, mock_timestamp_state.rx_ts_bytes, 5);
    }
}

void dwt_readtxtimestamp(uint8 *timestamp)
{
    mock_timestamp_state.tx_called++;
    if (timestamp != NULL) {
        memcpy(timestamp, mock_timestamp_state.tx_ts_bytes, 5);
    }
}

/* ---------------------------------------------------------------------------
 * Mock dwt_setdelayedtrxtime (link-time only -- see mock_deca_driver.h)
 * --------------------------------------------------------------------------- */
void dwt_setdelayedtrxtime(uint32 starttime)
{
    mock_delayedtrxtime_state.called    = 1;
    mock_delayedtrxtime_state.starttime = starttime;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_writetxdata (link-time only -- see mock_deca_driver.h)
 * --------------------------------------------------------------------------- */
int dwt_writetxdata(uint16 txFrameLength, uint8 *txFrameBytes, uint16 txBufferOffset)
{
    uint16 copy_len = txFrameLength;

    mock_tx_state.writetxdata_called = 1;
    mock_tx_state.writetxdata_len    = txFrameLength;
    mock_tx_state.writetxdata_offset = txBufferOffset;

    if (copy_len > MOCK_MAX_FRAME_LEN) {
        copy_len = MOCK_MAX_FRAME_LEN;
    }
    if (txFrameBytes != NULL) {
        memcpy(mock_tx_state.writetxdata_buf, txFrameBytes, copy_len);
    }

    return mock_tx_state.writetxdata_return_value;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_writetxfctrl (link-time only -- see mock_deca_driver.h)
 * --------------------------------------------------------------------------- */
void dwt_writetxfctrl(uint16 txFrameLength, uint16 txBufferOffset, int ranging)
{
    mock_tx_state.writetxfctrl_called  = 1;
    mock_tx_state.writetxfctrl_len     = txFrameLength;
    mock_tx_state.writetxfctrl_offset  = txBufferOffset;
    mock_tx_state.writetxfctrl_ranging = ranging;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_starttx (link-time only -- see mock_deca_driver.h)
 * --------------------------------------------------------------------------- */
int dwt_starttx(uint8 mode)
{
    mock_starttx_state.called = 1;
    mock_starttx_state.mode   = mode;
    return mock_starttx_state.return_value;
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
