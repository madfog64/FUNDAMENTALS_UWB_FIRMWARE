/*! ----------------------------------------------------------------------------
 * @file    test_image_health.c
 * @brief   ztest suite: boot self-check gate decision logic (UWB-266)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real hardware).
 *
 * Exercises only image_health_evaluate() -- the pure decision function.
 * image_health_run_checks() (the Zephyr/devicetree-dependent side that
 * gathers the real inputs) is not compiled into this test binary; it is
 * exercised by the real board build + the on-device bring-up checklist
 * (README.md).
 *
 * Covers, per the UWB-266 acceptance criteria ("host-testable gate
 * decision"):
 *   - All checks true -> OK.
 *   - Any single check false -> FAIL (kernel, log, dw1000_bus each in turn).
 *   - All checks false -> FAIL.
 *   - NULL input -> FAIL (defensive; must not crash).
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <zephyr/ztest.h>

#include "image_health.h"

ZTEST_SUITE(image_health, NULL, NULL, NULL, NULL, NULL);

static struct image_health_checks all_healthy(void)
{
	struct image_health_checks checks = {
		.kernel_ready = true,
		.log_ready = true,
		.dw1000_bus_ready = true,
	};

	return checks;
}

ZTEST(image_health, test_all_checks_pass_yields_ok)
{
	struct image_health_checks checks = all_healthy();

	zassert_equal(image_health_evaluate(&checks), IMAGE_HEALTH_OK,
		"all-healthy checks must pass the gate");
}

ZTEST(image_health, test_kernel_not_ready_fails_gate)
{
	struct image_health_checks checks = all_healthy();

	checks.kernel_ready = false;

	zassert_equal(image_health_evaluate(&checks), IMAGE_HEALTH_FAIL,
		"kernel_ready=false must fail the gate");
}

ZTEST(image_health, test_log_not_ready_fails_gate)
{
	struct image_health_checks checks = all_healthy();

	checks.log_ready = false;

	zassert_equal(image_health_evaluate(&checks), IMAGE_HEALTH_FAIL,
		"log_ready=false must fail the gate");
}

ZTEST(image_health, test_dw1000_bus_not_ready_fails_gate)
{
	struct image_health_checks checks = all_healthy();

	checks.dw1000_bus_ready = false;

	zassert_equal(image_health_evaluate(&checks), IMAGE_HEALTH_FAIL,
		"dw1000_bus_ready=false must fail the gate");
}

ZTEST(image_health, test_all_checks_false_fails_gate)
{
	struct image_health_checks checks = {
		.kernel_ready = false,
		.log_ready = false,
		.dw1000_bus_ready = false,
	};

	zassert_equal(image_health_evaluate(&checks), IMAGE_HEALTH_FAIL,
		"all-false checks must fail the gate");
}

ZTEST(image_health, test_null_checks_fails_gate)
{
	zassert_equal(image_health_evaluate(NULL), IMAGE_HEALTH_FAIL,
		"a NULL checks pointer must fail the gate, not crash");
}
