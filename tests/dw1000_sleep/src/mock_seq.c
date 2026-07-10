/*! ----------------------------------------------------------------------------
 * @file    mock_seq.c
 * @brief   Shared call-order sequence counter (UWB-316 ztest suite).
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include "mock_seq.h"

int mock_seq_counter;

void mock_seq_reset(void)
{
    mock_seq_counter = 0;
}

int mock_seq_next(void)
{
    return mock_seq_counter++;
}
