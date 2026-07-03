/*! ----------------------------------------------------------------------------
 * @file    mock_port.h
 * @brief   Shared state for the port function mocks (UWB-155 unit test).
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef MOCK_PORT_H_
#define MOCK_PORT_H_

#include "deca_device_api.h"

/** Call counters for port functions. */
struct mock_port_state {
    int reset_called;      /**< Number of times reset_DW1000() was called.             */
    int slowrate_called;   /**< Number of times port_set_dw1000_slowrate() was called.  */
    int fastrate_called;   /**< Number of times port_set_dw1000_fastrate() was called.  */
};

extern struct mock_port_state mock_port_state;

/** Reset call counters before each test. */
void mock_port_reset(void);

#endif /* MOCK_PORT_H_ */
