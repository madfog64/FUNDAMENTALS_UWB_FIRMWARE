/*! ----------------------------------------------------------------------------
 * @file    image_health_zephyr.c
 * @brief   Zephyr-side gathering of the boot-time image-confirm gate checks
 *          (UWB-266, ADR-0040).
 *
 * Populates a struct image_health_checks from real kernel/devicetree state
 * and delegates the pass/fail decision to image_health_evaluate() (kept pure
 * and host-testable in image_health.c). This file is Zephyr-only and is not
 * part of tests/image_health's host-test build.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "image_health.h"

LOG_MODULE_REGISTER(image_health, CONFIG_LOG_DEFAULT_LEVEL);

/* ---------------------------------------------------------------------------
 * Devicetree node -- same node deca_port.c uses (boards/nrf52dk_nrf52832.overlay:
 *   dw1000: dw1000@0 { compatible = "decawave,dw1000"; ... };
 *
 * Only the SPI bus controller and the reset/int GPIO controllers are checked
 * here (device_is_ready()/gpio_is_ready_dt()) -- a presence/wiring check,
 * not a DW1000 register access. dw1000_port_init()/dw1000_configure() (which
 * do talk to the chip) are not yet wired into src/main.c as of UWB-266 --
 * see the TODO in image_health.c.
 * --------------------------------------------------------------------------- */
#define DW1000_NODE DT_NODELABEL(dw1000)

BUILD_ASSERT(DT_NODE_EXISTS(DW1000_NODE),
	     "DT node 'dw1000' not found — check boards/nrf52dk_nrf52832.overlay");

#define DW1000_SPI_OP (SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB)

static const struct spi_dt_spec dw1000_spi = SPI_DT_SPEC_GET(DW1000_NODE, DW1000_SPI_OP, 0);
static const struct gpio_dt_spec dw1000_reset_gpio = GPIO_DT_SPEC_GET(DW1000_NODE, reset_gpios);
static const struct gpio_dt_spec dw1000_int_gpio = GPIO_DT_SPEC_GET(DW1000_NODE, int_gpios);

static bool dw1000_bus_is_ready(void)
{
	if (!device_is_ready(dw1000_spi.bus)) {
		LOG_ERR("DW1000 SPI bus device not ready");
		return false;
	}

	if (!gpio_is_ready_dt(&dw1000_reset_gpio)) {
		LOG_ERR("DW1000 reset GPIO controller not ready");
		return false;
	}

	if (!gpio_is_ready_dt(&dw1000_int_gpio)) {
		LOG_ERR("DW1000 IRQ GPIO controller not ready");
		return false;
	}

	return true;
}

enum image_health_result image_health_run_checks(void)
{
	struct image_health_checks checks = {
		/* Reaching this call means the kernel scheduler is up and
		 * main() was entered -- the strongest "kernel ready" evidence
		 * available this early in boot.
		 */
		.kernel_ready = true,

		/* Zephyr logging initialises via SYS_INIT before main() runs
		 * (CONFIG_LOG=y + CONFIG_LOG_BACKEND_RTT/_UART, prj.conf);
		 * every LOG_*() call in this file having been emitted is
		 * itself the evidence.
		 */
		.log_ready = true,

		.dw1000_bus_ready = dw1000_bus_is_ready(),
	};

	enum image_health_result result = image_health_evaluate(&checks);

	LOG_INF("Boot self-check: kernel=%d log=%d dw1000_bus=%d -> %s",
		checks.kernel_ready, checks.log_ready, checks.dw1000_bus_ready,
		result == IMAGE_HEALTH_OK ? "OK" : "FAIL");

	return result;
}
