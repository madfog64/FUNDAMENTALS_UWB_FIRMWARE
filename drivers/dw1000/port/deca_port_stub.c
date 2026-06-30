/*! ----------------------------------------------------------------------------
 * @file    deca_port_stub.c
 * @brief   Weak stub implementations of the Decawave DW1000 platform functions
 *          for the Zephyr port.
 *
 * These stubs satisfy the linker until UWB-93 provides the real Zephyr SPI
 * and interrupt integration.  Every symbol is marked __weak so that a
 * non-weak definition in the UWB-93 port source automatically takes
 * precedence at link time without requiring changes to this file.
 *
 * Stub behaviour and safety notes:
 *
 *   writetospi / readfromspi
 *       Return DWT_ERROR immediately.  The DW1000 MUST NOT be accessed before
 *       UWB-93 replaces these with real Zephyr SPI transactions.
 *
 *   decamutexon / decamutexoff
 *       No-ops.  Critical sections around the DW1000 ISR are NOT guarded
 *       until UWB-93 supplies irq_lock() / irq_unlock() implementations.
 *
 *   deca_sleep
 *       Delegates to k_msleep().  This is the correct Zephyr implementation
 *       and is unlikely to change in UWB-93.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <zephyr/kernel.h>

#include "deca_device_api.h"

int __weak writetospi(uint16 headerLength, const uint8 *headerBuffer,
                      uint32 bodylength, const uint8 *bodyBuffer)
{
    /* Stub: implemented by UWB-93 (Zephyr SPI port). */
    ARG_UNUSED(headerLength);
    ARG_UNUSED(headerBuffer);
    ARG_UNUSED(bodylength);
    ARG_UNUSED(bodyBuffer);
    return DWT_ERROR;
}

int __weak readfromspi(uint16 headerLength, const uint8 *headerBuffer,
                       uint32 readlength, uint8 *readBuffer)
{
    /* Stub: implemented by UWB-93 (Zephyr SPI port). */
    ARG_UNUSED(headerLength);
    ARG_UNUSED(headerBuffer);
    ARG_UNUSED(readlength);
    ARG_UNUSED(readBuffer);
    return DWT_ERROR;
}

decaIrqStatus_t __weak decamutexon(void)
{
    /* Stub: interrupts are NOT disabled. UWB-93 will use irq_lock(). */
    return 0;
}

void __weak decamutexoff(decaIrqStatus_t s)
{
    /* Stub: interrupts are NOT restored. UWB-93 will use irq_unlock(). */
    ARG_UNUSED(s);
}

void __weak deca_sleep(unsigned int time_ms)
{
    /* Delegate to Zephyr scheduler sleep — correct Zephyr implementation. */
    k_msleep((int32_t)time_ms);
}
