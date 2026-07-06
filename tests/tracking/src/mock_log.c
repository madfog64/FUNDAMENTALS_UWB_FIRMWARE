/*! ----------------------------------------------------------------------------
 * @file    mock_log.c
 * @brief   Zephyr minimal-log stub for the uwb_tracking unit test (UWB-252).
 *
 * Copied from tests/dw1000_sync/src/mock_log.c (UWB-242) / originally
 * tests/dw1000_ranging/src/mock_log.c (UWB-231): the tracking test build
 * compiles dw1000_ranging.c, dw1000_sync.c, uwb_cycle_ref.c, and
 * uwb_slot_timing.c alongside uwb_tracking.c itself, and the unit_testing
 * defconfig enables CONFIG_LOG=y, which causes every LOG_ERR()/LOG_DBG()/
 * LOG_WRN() macro in those modules to call z_log_minimal_printk() at link
 * time. That function lives in Zephyr's log_minimal.c, which is not compiled
 * in the minimal unit_testing link set. Stub it out here — unit tests do not
 * need real log output, only assertion correctness.
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
