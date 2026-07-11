/*! ----------------------------------------------------------------------------
 * @file    mock_seq.h
 * @brief   Shared call-order sequence counter for the uwb_standby ztest
 *          suite (UWB-319). Copied verbatim from
 *          tests/dw1000_sleep/src/mock_seq.h (UWB-316).
 *
 * mock_deca_driver.c and mock_port.c are separate compilation units that
 * both need to stamp captured calls with a monotonically increasing sequence
 * number, so the test assertions can verify call ORDER and not just call
 * presence/count.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef MOCK_SEQ_H_
#define MOCK_SEQ_H_

/** Monotonically increasing counter; mock_seq_next() returns then increments. */
extern int mock_seq_counter;

/** Reset the counter to 0. Call before each test (mock_seq_reset()). */
void mock_seq_reset(void);

/** Return the next sequence number (0, 1, 2, ...). */
int mock_seq_next(void);

#endif /* MOCK_SEQ_H_ */
