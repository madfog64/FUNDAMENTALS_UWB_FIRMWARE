/*! ----------------------------------------------------------------------------
 * @file    mock_port.h
 * @brief   Shared state for the port function mocks (UWB-316 ztest suite).
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef MOCK_PORT_H_
#define MOCK_PORT_H_

#include "deca_device_api.h"

/** Call counters + call-order sequence numbers for port functions. */
struct mock_port_state {
    int reset_called;
    int reset_seq;

    int slowrate_called;
    int fastrate_called;

    /** port_wakeup_dw1000() — the WAKEUP pin toggle (UWB-316). */
    int wakeup_called;
    int wakeup_seq;
};

extern struct mock_port_state mock_port_state;

/** Reset call counters before each test. */
void mock_port_reset(void);

#endif /* MOCK_PORT_H_ */
