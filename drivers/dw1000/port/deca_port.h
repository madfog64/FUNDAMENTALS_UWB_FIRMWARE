/*! ----------------------------------------------------------------------------
 * @file    deca_port.h
 * @brief   Zephyr port shim for the vendored Decawave DW1000 driver.
 *
 * This header is the boundary between the vendored deca_driver/ sources and
 * the Zephyr platform.  The five platform-facing symbols required by the
 * driver are declared in the vendored deca_device_api.h:
 *
 *   writetospi()     SPI write transaction
 *   readfromspi()    SPI read transaction
 *   decamutexon()    disable DW1000 interrupts, return saved state
 *   decamutexoff()   restore interrupt state
 *   deca_sleep()     millisecond busy/sleep delay
 *
 * Stub implementations live in deca_port_stub.c and are marked __weak so
 * that UWB-93 (Zephyr SPI / IRQ integration) can override them with strong
 * definitions without modifying this file.
 *
 * UWB-93 will:
 *   - implement writetospi / readfromspi using the Zephyr SPI API
 *     (CONFIG_SPI, struct spi_dt_spec)
 *   - implement decamutexon / decamutexoff using irq_lock() / irq_unlock()
 *   - leave deca_sleep unchanged (k_msleep is correct for Zephyr)
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

#include <zephyr/kernel.h>

/*
 * Include the vendored API so that callers only need to include this one
 * header to get both the DW1000 API and the port declarations.
 */
#include "deca_device_api.h"

#ifdef __cplusplus
}
#endif

#endif /* DECA_PORT_H_ */
