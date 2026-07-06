/*! ----------------------------------------------------------------------------
 * @file    mock_log.c
 * @brief   Zephyr minimal-log stub for the uwb_cycle_ref unit test (UWB-243).
 *
 * Copied from tests/dw1000_sync/src/mock_log.c (UWB-242): the unit_testing
 * defconfig enables CONFIG_LOG=y, which causes the LOG_WRN() macro in
 * uwb_cycle_ref.c (cycle_seq discontinuity warning) to call
 * z_log_minimal_printk() at link time. That function lives in Zephyr's
 * log_minimal.c, which is not compiled in the minimal unit_testing link set.
 * Stub it out here — unit tests do not need real log output, only assertion
 * correctness (the discontinuity warning path is still exercised; only the
 * actual print is discarded).
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <stdarg.h>

void z_log_minimal_printk(const char *fmt, ...)
{
    (void)fmt;
    /* No-op: discard log messages in unit tests. */
}
