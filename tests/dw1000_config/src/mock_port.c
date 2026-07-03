/*! ----------------------------------------------------------------------------
 * @file    mock_port.c
 * @brief   Mock implementations of port functions called by dw1000_configure()
 *          (UWB-155 unit test).
 *
 * These stubs replace deca_port.c in the unit_testing build.  They are
 * no-ops with call counters so the test can verify the expected call sequence.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <string.h>
#include "mock_port.h"

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
}

void port_set_dw1000_slowrate(void)
{
    mock_port_state.slowrate_called++;
}

void port_set_dw1000_fastrate(void)
{
    mock_port_state.fastrate_called++;
}

/* ---------------------------------------------------------------------------
 * deca_driver SPI/mutex stubs — required at link time by the mock driver.
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
