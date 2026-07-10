/*! ----------------------------------------------------------------------------
 * @file    image_health.c
 * @brief   Pure decision logic for the boot-time image-confirm gate (UWB-266).
 *
 * Deliberately free of any Zephyr/kernel/devicetree include -- this is the
 * unit-test seam (tests/image_health) for the gate's pass/fail decision.
 * The hardware-facing counterpart that gathers the real inputs is
 * image_health_zephyr.c (image_health_run_checks()).
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include "image_health.h"

enum image_health_result image_health_evaluate(const struct image_health_checks *checks)
{
	if (checks == NULL) {
		return IMAGE_HEALTH_FAIL;
	}

	if (!checks->kernel_ready || !checks->log_ready || !checks->dw1000_bus_ready) {
		return IMAGE_HEALTH_FAIL;
	}

	/*
	 * TODO(follow-on, once UWB-91/UWB-92 wire dw1000_port_init() +
	 * dw1000_configure() into src/main.c): extend this gate with an
	 * actual "DW1000 responds" check (e.g. a successful device-ID
	 * register read after dwt_initialise()) instead of only the
	 * bus/GPIO presence check gathered into checks->dw1000_bus_ready.
	 * Deliberately NOT done here -- UWB-266 scope is explicit: "do NOT
	 * block on full UWB bring-up."
	 */

	return IMAGE_HEALTH_OK;
}
