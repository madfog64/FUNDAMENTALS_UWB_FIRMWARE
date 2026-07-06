/*! ----------------------------------------------------------------------------
 * @file    mock_log.c
 * @brief   Zephyr minimal-log stub for the uwb_slot_timing unit test
 *          (UWB-251).
 *
 * Mirrors tests/dw1000_ranging/src/mock_log.c: the unit_testing defconfig
 * enables CONFIG_LOG=y, which causes the LOG_ERR() / LOG_DBG() macros in the
 * linked-in dw1000_ranging.c to call z_log_minimal_printk() at link time.
 * That function lives in Zephyr's log_minimal.c, which is not compiled in
 * the minimal unit_testing link set. Stub it out here — unit tests do not
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
