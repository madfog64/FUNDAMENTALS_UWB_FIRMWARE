/*! ----------------------------------------------------------------------------
 * @file    mock_log.c
 * @brief   Zephyr minimal-log stub for the uwb_join unit test (UWB-261).
 *
 * Copied from tests/tracking/src/mock_log.c (UWB-252) / originally
 * tests/dw1000_sync/src/mock_log.c (UWB-242) / tests/dw1000_ranging/src/mock_log.c
 * (UWB-231): the join test build compiles dw1000_ranging.c and dw1000_sync.c
 * alongside uwb_join.c itself, and the unit_testing defconfig enables
 * CONFIG_LOG=y, which causes every LOG_ERR()/LOG_DBG()/LOG_WRN()/LOG_INF()
 * macro in those modules to call z_log_minimal_printk() at link time. That
 * function lives in Zephyr's log_minimal.c, which is not compiled in the
 * minimal unit_testing link set. Stub it out here — unit tests do not need
 * real log output, only assertion correctness.
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
