/*! ----------------------------------------------------------------------------
 * @file    mock_deca_driver.c
 * @brief   Mock implementations of deca_driver API functions called by
 *          dw1000_ranging.c (UWB-231) and exercised transitively by
 *          twr_responder.c (UWB-232 unit tests).
 *
 * These stubs have no hardware or SPI dependencies. See mock_deca_driver.h
 * for the RX-queue rationale (a single twr_responder_run_once() attempt can
 * call dw1000_rx() up to twice).
 *
 * RX side: dwt_rxenable() advances mock_rx_queue_state to the next
 * programmed mock_rx_event (or a default frame-wait-timeout event if the
 * queue is exhausted); SYS_STATUS / RX_FINFO / dwt_readrxdata() /
 * dwt_readrxtimestamp() all read from that active event until the next
 * dwt_rxenable() call.
 *
 * TX side: unchanged single-shot mocking (dw1000_tx_at() is called at most
 * once per attempt) -- dwt_setdelayedtrxtime() / dwt_writetxdata() /
 * dwt_writetxfctrl() / dwt_starttx() capture their arguments directly.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <string.h>

#include "deca_device_api.h"
#include "deca_regs.h"
#include "mock_deca_driver.h"

/* ---------------------------------------------------------------------------
 * Captured / configurable state (inspected and pre-configured by tests)
 * --------------------------------------------------------------------------- */
struct mock_rx_queue_state        mock_rx_queue_state;
struct mock_rxtimeout_state       mock_rxtimeout_state;
struct mock_rxenable_state        mock_rxenable_state;
struct mock_forcetrxoff_state     mock_forcetrxoff_state;
struct mock_reg_state             mock_reg_state;
struct mock_readrxdata_state      mock_readrxdata_state;
struct mock_timestamp_state       mock_timestamp_state;
struct mock_delayedtrxtime_state  mock_delayedtrxtime_state;
struct mock_tx_state              mock_tx_state;
struct mock_starttx_state         mock_starttx_state;

/* Default event served when dwt_rxenable() is called more times than the
 * test programmed via mock_rx_queue_push(): a frame-wait timeout. */
static const struct mock_rx_event mock_rx_default_timeout_event = {
    .sys_status = SYS_STATUS_RXRFTO,
    .rx_finfo = 0u,
    .payload = {0},
    .payload_len = 0u,
    .rx_ts_bytes = {0, 0, 0, 0, 0},
};

/* The event the currently in-flight dw1000_rx() call (since its last
 * dwt_rxenable()) should be served from -- backs RX_FINFO / dwt_readrxdata()
 * / dwt_readrxtimestamp(), which are only read once RXFCG is already latched
 * (see mock_current_sys_status below for why SYS_STATUS itself is NOT read
 * from this directly). */
static const struct mock_rx_event *mock_rx_active = &mock_rx_default_timeout_event;

/*
 * The single "live" SYS_STATUS register value, shared by BOTH polling loops
 * in dw1000_ranging.c: dw1000_rx()'s good-frame/error/timeout poll AND
 * dw1000_tx_at()'s post-dwt_starttx() TXFRS poll (both read SYS_STATUS_ID via
 * the same dwt_read32bitreg() macro). It must therefore behave like the real
 * hardware register: whichever bits are relevant to the operation in
 * progress, with write-1-to-clear semantics.
 *
 *   - dwt_rxenable() (start of a dw1000_rx() call) loads the active RX
 *     event's sys_status here -- this is what dw1000_rx()'s polling loop
 *     will observe.
 *   - dwt_starttx() OR's in SYS_STATUS_TXFRS on success, simulating the
 *     DW1000 completing the (mocked, instantaneous) transmission by the time
 *     dw1000_tx_at()'s post-dwt_starttx() poll runs.
 *   - dwt_write32bitoffsetreg(SYS_STATUS_ID, ...) clears the written bits,
 *     matching real DW1000 write-1-to-clear semantics.
 *
 * Without this, a mock that only tracked the "active RX event" would leave
 * dw1000_tx_at()'s TXFRS poll spinning forever after a POLL RX event with
 * SYS_STATUS_RXFCG (no TXFRS bit) was left active from the preceding
 * dw1000_rx() call.
 */
static uint32 mock_current_sys_status;

void mock_deca_reset(void)
{
    memset(&mock_rx_queue_state,       0, sizeof(mock_rx_queue_state));
    memset(&mock_rxtimeout_state,      0, sizeof(mock_rxtimeout_state));
    memset(&mock_rxenable_state,       0, sizeof(mock_rxenable_state));
    memset(&mock_forcetrxoff_state,    0, sizeof(mock_forcetrxoff_state));
    memset(&mock_reg_state,            0, sizeof(mock_reg_state));
    memset(&mock_readrxdata_state,     0, sizeof(mock_readrxdata_state));
    memset(&mock_timestamp_state,      0, sizeof(mock_timestamp_state));
    memset(&mock_delayedtrxtime_state, 0, sizeof(mock_delayedtrxtime_state));
    memset(&mock_tx_state,             0, sizeof(mock_tx_state));
    memset(&mock_starttx_state,        0, sizeof(mock_starttx_state));

    mock_rx_active = &mock_rx_default_timeout_event;
    mock_current_sys_status = 0u;

    /* Defaults: every deca_driver call that can fail reports success. */
    mock_rxenable_state.return_value       = DWT_SUCCESS;
    mock_tx_state.writetxdata_return_value = DWT_SUCCESS;
    mock_starttx_state.return_value        = DWT_SUCCESS;
}

void mock_rx_queue_push(uint32 sys_status, uint32 rx_finfo,
                         const uint8 *payload, uint16 payload_len,
                         const uint8 rx_ts_bytes[5])
{
    struct mock_rx_event *ev;
    uint16 copy_len;

    if (mock_rx_queue_state.count >= MOCK_RX_QUEUE_DEPTH) {
        return; /* test authoring error -- silently drop rather than overflow */
    }

    ev = &mock_rx_queue_state.events[mock_rx_queue_state.count];
    memset(ev, 0, sizeof(*ev));

    ev->sys_status = sys_status;
    ev->rx_finfo = rx_finfo;
    ev->payload_len = payload_len;

    if (payload != NULL && payload_len > 0u) {
        copy_len = payload_len;
        if (copy_len > MOCK_MAX_FRAME_LEN) {
            copy_len = MOCK_MAX_FRAME_LEN;
        }
        memcpy(ev->payload, payload, copy_len);
    }

    if (rx_ts_bytes != NULL) {
        memcpy(ev->rx_ts_bytes, rx_ts_bytes, 5);
    }

    mock_rx_queue_state.count++;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_setrxtimeout
 * --------------------------------------------------------------------------- */
void dwt_setrxtimeout(uint16 time)
{
    mock_rxtimeout_state.called++;
    mock_rxtimeout_state.time = time;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_rxenable -- also advances the RX event queue
 * --------------------------------------------------------------------------- */
int dwt_rxenable(int mode)
{
    mock_rxenable_state.called++;
    mock_rxenable_state.mode = mode;

    if (mock_rx_queue_state.next_index < mock_rx_queue_state.count) {
        mock_rx_active = &mock_rx_queue_state.events[mock_rx_queue_state.next_index];
        mock_rx_queue_state.next_index++;
    } else {
        mock_rx_active = &mock_rx_default_timeout_event;
    }

    /* Load the live SYS_STATUS register with this attempt's outcome -- see
     * mock_current_sys_status's doc comment. */
    mock_current_sys_status = mock_rx_active->sys_status;

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
 * Returns the live SYS_STATUS register for SYS_STATUS_ID (see
 * mock_current_sys_status's doc comment -- shared by dw1000_rx()'s and
 * dw1000_tx_at()'s polling loops), the active RX event's rx_finfo for
 * RX_FINFO_ID, 0 for anything else.
 * --------------------------------------------------------------------------- */
uint32 dwt_read32bitoffsetreg(int regFileID, int regOffset)
{
    (void)regOffset;

    if (regFileID == SYS_STATUS_ID) {
        return mock_current_sys_status;
    }
    if (regFileID == RX_FINFO_ID) {
        return mock_rx_active->rx_finfo;
    }
    return 0u;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_write32bitoffsetreg (backs the dwt_write32bitreg() macro)
 *
 * Captures the last register/value written, and -- for SYS_STATUS_ID --
 * clears the written bits from the live SYS_STATUS register
 * (mock_current_sys_status), matching real DW1000 write-1-to-clear semantics.
 * --------------------------------------------------------------------------- */
void dwt_write32bitoffsetreg(int regFileID, int regOffset, uint32 regval)
{
    (void)regOffset;

    mock_reg_state.last_write_reg = (uint32)regFileID;
    mock_reg_state.last_write_val = regval;
    mock_reg_state.write_count++;

    if (regFileID == SYS_STATUS_ID) {
        mock_current_sys_status &= ~regval;
    }
}

/* ---------------------------------------------------------------------------
 * Mock dwt_readrxdata
 *
 * Captures length/offset and copies the active RX event's payload bytes into
 * the caller's buffer.
 * --------------------------------------------------------------------------- */
void dwt_readrxdata(uint8 *buffer, uint16 length, uint16 rxBufferOffset)
{
    uint16 copy_len = length;

    mock_readrxdata_state.called++;
    mock_readrxdata_state.length = length;
    mock_readrxdata_state.offset = rxBufferOffset;

    if (copy_len > MOCK_MAX_FRAME_LEN) {
        copy_len = MOCK_MAX_FRAME_LEN;
    }
    if (buffer != NULL) {
        memcpy(buffer, mock_rx_active->payload, copy_len);
    }
}

/* ---------------------------------------------------------------------------
 * Mock dwt_readrxtimestamp / dwt_readtxtimestamp
 * --------------------------------------------------------------------------- */
void dwt_readrxtimestamp(uint8 *timestamp)
{
    mock_timestamp_state.rx_called++;
    if (timestamp != NULL) {
        memcpy(timestamp, mock_rx_active->rx_ts_bytes, 5);
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
 * Mock dwt_setdelayedtrxtime
 * --------------------------------------------------------------------------- */
void dwt_setdelayedtrxtime(uint32 starttime)
{
    mock_delayedtrxtime_state.called++;
    mock_delayedtrxtime_state.starttime = starttime;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_writetxdata
 * --------------------------------------------------------------------------- */
int dwt_writetxdata(uint16 txFrameLength, uint8 *txFrameBytes, uint16 txBufferOffset)
{
    uint16 copy_len = txFrameLength;

    mock_tx_state.writetxdata_called++;
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
 * Mock dwt_writetxfctrl
 * --------------------------------------------------------------------------- */
void dwt_writetxfctrl(uint16 txFrameLength, uint16 txBufferOffset, int ranging)
{
    mock_tx_state.writetxfctrl_called++;
    mock_tx_state.writetxfctrl_len     = txFrameLength;
    mock_tx_state.writetxfctrl_offset  = txBufferOffset;
    mock_tx_state.writetxfctrl_ranging = ranging;
}

/* ---------------------------------------------------------------------------
 * Mock dwt_starttx
 *
 * On success, latches SYS_STATUS_TXFRS into the live SYS_STATUS register
 * (mock_current_sys_status) so dw1000_tx_at()'s post-dwt_starttx() polling
 * loop observes the transmission as instantly complete. On failure
 * (DWT_ERROR, simulating HPDWARN), leaves the register untouched --
 * dw1000_tx_at() does not enter the TXFRS wait loop in that case.
 * --------------------------------------------------------------------------- */
int dwt_starttx(uint8 mode)
{
    mock_starttx_state.called++;
    mock_starttx_state.mode = mode;

    if (mock_starttx_state.return_value == DWT_SUCCESS) {
        mock_current_sys_status |= SYS_STATUS_TXFRS;
    }

    return mock_starttx_state.return_value;
}
