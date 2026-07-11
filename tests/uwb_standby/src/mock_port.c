/*! ----------------------------------------------------------------------------
 * @file    mock_port.c
 * @brief   Mock implementations of port functions called by dw1000_sleep() /
 *          dw1000_wake() and the dw1000_configure() path dw1000_wake()
 *          drives (UWB-319 ztest suite). Copied verbatim from
 *          tests/dw1000_sleep/src/mock_port.c (UWB-316).
 *
 * These stubs replace deca_port.c in the unit_testing build. They are
 * no-ops with call counters + call-order sequence numbers (mock_seq.h) so
 * the test can verify the expected call sequence.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <string.h>
#include "mock_port.h"
#include "mock_seq.h"

/* ---------------------------------------------------------------------------
 * Captured call state
 * --------------------------------------------------------------------------- */
struct mock_port_state mock_port_state;

void mock_port_reset(void)
{
    memset(&mock_port_state, 0, sizeof(mock_port_state));
}

/* ---------------------------------------------------------------------------
 * Port function stubs
 * --------------------------------------------------------------------------- */

void reset_DW1000(void)
{
    mock_port_state.reset_called++;
    mock_port_state.reset_seq = mock_seq_next();
}

void port_set_dw1000_slowrate(void)
{
    mock_port_state.slowrate_called++;
}

void port_set_dw1000_fastrate(void)
{
    mock_port_state.fastrate_called++;
}

/**
 * @brief Mock WAKEUP pin toggle (UWB-316).
 *
 * The real implementation (deca_port.c) drives a GPIO and busy-waits/sleeps;
 * this mock only records that it was called and where in the call sequence,
 * so the test can assert the WAKEUP toggle happens before the
 * reconfigure-on-wake sequence (dwt_initialise/dwt_configure).
 */
void port_wakeup_dw1000(void)
{
    mock_port_state.wakeup_called++;
    mock_port_state.wakeup_seq = mock_seq_next();
}

/* ---------------------------------------------------------------------------
 * deca_driver SPI/mutex stubs -- required at link time by the mock driver.
 * --------------------------------------------------------------------------- */

int writetospi(uint16 headerLength, const uint8 *headerBuffer,
               uint32 bodylength, const uint8 *bodyBuffer)
{
    (void)headerLength; (void)headerBuffer;
    (void)bodylength;   (void)bodyBuffer;
    return DWT_SUCCESS;
}

int readfromspi(uint16 headerLength, const uint8 *headerBuffer,
                uint32 readlength, uint8 *readBuffer)
{
    (void)headerLength; (void)headerBuffer;
    (void)readlength;   (void)readBuffer;
    return DWT_SUCCESS;
}

decaIrqStatus_t decamutexon(void)  { return 0; }
void decamutexoff(decaIrqStatus_t s) { (void)s; }
void deca_sleep(unsigned int time_ms) { (void)time_ms; }

/* ---------------------------------------------------------------------------
 * Zephyr logging stubs.
 *
 * The unit_testing defconfig enables CONFIG_LOG=y which causes LOG_ERR() /
 * LOG_DBG() / LOG_INF() macros in dw1000_config.c / dw1000_sleep.c /
 * dw1000_ranging.c / dw1000_sync.c / uwb_standby.c to call
 * z_log_minimal_printk() at link time. That function lives in Zephyr's
 * log_minimal.c which is not compiled in the minimal unit_testing link set.
 * Stub it out here -- unit tests do not need real log output, only assertion
 * correctness.
 * --------------------------------------------------------------------------- */
#include <stdarg.h>

void z_log_minimal_printk(const char *fmt, ...)
{
    (void)fmt;
    /* No-op: discard log messages in unit tests. */
}
