/*! ----------------------------------------------------------------------------
 * @file    deca_port.h
 * @brief   Zephyr port shim for the vendored Decawave DW1000 driver.
 *
 * This header is the boundary between the vendored deca_driver/ sources and
 * the Zephyr platform.  It re-exports deca_device_api.h and declares the
 * port-specific functions provided by deca_port.c (UWB-93).
 *
 * The five platform-facing symbols required by the vendored driver are
 * declared in deca_device_api.h:
 *
 *   writetospi()     SPI write transaction
 *   readfromspi()    SPI read transaction
 *   decamutexon()    disable interrupts, return saved irq lock key
 *   decamutexoff()   restore interrupt state
 *   deca_sleep()     millisecond sleep (delegates to k_msleep)
 *
 * Strong definitions for all five symbols live in deca_port.c.  The __weak
 * stubs in deca_port_stub.c are silently discarded at link time.
 *
 * Additional port functions (rate switching, reset, IRQ control, tick
 * counter) are declared here and implemented in deca_port.c.  Application
 * code should call dw1000_port_init() once during boot before using any
 * other port function or the deca_driver API.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef DECA_PORT_H_
#define DECA_PORT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <zephyr/kernel.h>

/*
 * Include the vendored API so that callers only need this header to access
 * both the DW1000 device API and the port declarations.
 */
#include "deca_device_api.h"

/* ---------------------------------------------------------------------------
 * Port initialisation
 *
 * Must be called once before any deca_driver or port function.  Sets up:
 *   - slow and fast SPI configs (g_spi starts slow for dwt_initialise)
 *   - RESET GPIO as output, de-asserted (DW1000 not in reset)
 *   - IRQ GPIO as input (interrupt not yet enabled)
 *   - work queue item for deferred dwt_isr() execution
 *
 * Returns 0 on success or a negative errno on failure.
 * --------------------------------------------------------------------------- */
int dw1000_port_init(void);

/* ---------------------------------------------------------------------------
 * SPI rate switching
 *
 * DW1000 requirement (User Manual section 2.3.2):
 *   - SPI clock must be < 3 MHz during dwt_initialise().
 *   - After successful initialisation the clock may be raised.
 *
 * Typical call sequence:
 *   dw1000_port_init();
 *   reset_DW1000();
 *   port_set_dw1000_slowrate();      // 2 MHz
 *   dwt_initialise(DWT_LOADUCODE);
 *   port_set_dw1000_fastrate();      // 8 MHz
 * --------------------------------------------------------------------------- */
void port_set_dw1000_slowrate(void);   /* < 3 MHz  — required during dwt_initialise */
void port_set_dw1000_fastrate(void);   /* 8 MHz    — switch after dwt_initialise    */

/* ---------------------------------------------------------------------------
 * Hardware reset
 *
 * Drives RESET low (asserts), waits, then releases to input so the DW1000's
 * internal open-drain pull-up can bring the line high.  Function is re-entrant.
 * --------------------------------------------------------------------------- */
void reset_DW1000(void);

/* ---------------------------------------------------------------------------
 * IRQ enable / disable
 *
 * Controls whether the rising-edge interrupt on irq-gpios triggers
 * the work-queue dispatch of dwt_isr().  Call port_enable_dw1000_irq()
 * after dwt_setinterrupt() has been configured.
 * --------------------------------------------------------------------------- */
void port_enable_dw1000_irq(void);
void port_disable_dw1000_irq(void);

/* ---------------------------------------------------------------------------
 * Delay and tick helpers
 * --------------------------------------------------------------------------- */

/**
 * @brief  Millisecond delay (alias for deca_sleep / k_msleep).
 * @param  time_ms  Duration in milliseconds.
 */
void Sleep(uint32_t time_ms);

/**
 * @brief  Return the elapsed time since boot in milliseconds.
 * @return 32-bit millisecond tick counter (wraps after ~49.7 days).
 */
uint32_t portGetTickCnt(void);

#ifdef __cplusplus
}
#endif

#endif /* DECA_PORT_H_ */
