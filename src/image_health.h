/*! ----------------------------------------------------------------------------
 * @file    image_health.h
 * @brief   Boot-time self-check gate for the MCUboot image-confirm hook
 *          (UWB-266, ADR-0040).
 *
 * A freshly-updated image must prove itself before the app marks it
 * permanent (boot_write_img_confirmed(), <zephyr/dfu/mcuboot.h>). This module
 * is the health-check gate that decides whether that call happens.
 *
 * Scope (deliberately small and explicit, per UWB-266):
 *   - "core subsystems initialised" -- kernel is running, logging is up, the
 *     DW1000 SPI bus + control GPIOs are present and Zephyr-ready.
 *   - NOT full UWB/DW1000 bring-up (no dwt_initialise(), no register read,
 *     no ranging). That is a documented TODO -- see
 *     image_health_run_checks() in image_health_zephyr.c -- for once tag
 *     DW1000 bring-up (UWB-91/UWB-92) is wired into src/main.c.
 *
 * Split across two translation units so the pass/fail *decision* has a
 * unit-test seam independent of hardware:
 *   image_health.c         image_health_evaluate() -- pure logic, no Zephyr
 *                           includes, host-testable (see tests/image_health).
 *   image_health_zephyr.c  image_health_run_checks() -- gathers the real
 *                           checks from kernel/devicetree state (Zephyr-only,
 *                           not unit tested; exercised by build + on-device
 *                           bring-up only).
 *
 * IMPORTANT -- overwrite-only swap mode (sysbuild.conf, UWB-264):
 *   MCUboot is configured SB_CONFIG_MCUBOOT_MODE_OVERWRITE_ONLY=y. In this
 *   mode MCUboot has NO test/revert state machine at all: the primary slot
 *   is unconditionally erased and overwritten from the secondary slot
 *   (boot_perform_update() -> boot_copy_image(), bootutil/loader.c), and the
 *   image_ok/copy_done trailer bookkeeping that swap-with-test modes use to
 *   decide "revert on next boot" is compiled out entirely
 *   (#ifndef MCUBOOT_OVERWRITE_ONLY guards in bootutil_misc.c/loader.c). Once
 *   an image has been overwritten into the primary slot there is no backup
 *   copy anywhere to revert to.
 *
 *   Practical consequence: this health gate + boot_write_img_confirmed()
 *   cannot make MCUboot roll back a bad-but-bootable image in this repo's
 *   configuration. What it DOES do:
 *     - Gate confirmation on a real (if narrow) health check, so a truly
 *       broken image (crashes/hangs before reaching the gate) never marks
 *       itself confirmed -- visible over `mcumgr image list` as
 *       "confirmed": false indefinitely.
 *     - Avoid a reboot-loop "fix": since there is no previous image to fall
 *       back to, this gate deliberately does NOT reboot on failure (see
 *       image_health_zephyr.c) -- that would just busy-loop the same bad
 *       image forever with no path to recovery. Instead it logs loudly and
 *       lets the app continue in an unconfirmed state, keeping the BLE/SMP
 *       transport (UWB-265) alive so a corrected image can be uploaded.
 *   The sysbuild.conf comment on this trade-off ("MCUboot's overwrite-only
 *   revert bookkeeping is the only safety net") is corrected by this file:
 *   overwrite-only mode has no such bookkeeping. See README.md "MCUboot &
 *   OTA" for the user-facing writeup of this limitation and the hardware
 *   bring-up checklist implication.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef IMAGE_HEALTH_H_
#define IMAGE_HEALTH_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Boot-time self-check inputs for the image-confirm gate.
 *
 * Plain booleans by design: keeps image_health_evaluate() a pure function of
 * its input (no Zephyr kernel/device dependency), so it is host-testable
 * (ztest/native, see tests/image_health) independent of real hardware.
 */
struct image_health_checks {
	/** Reached main()'s health-check call with the kernel scheduler
	 *  running. Always true in image_health_run_checks() -- included so
	 *  the struct documents everything the gate considers, and so a unit
	 *  test can exercise the "kernel not ready" branch without a real
	 *  kernel. */
	bool kernel_ready;

	/** Zephyr logging is initialised (CONFIG_LOG=y + at least one backend,
	 *  prj.conf). Always true in image_health_run_checks() for the same
	 *  reason as kernel_ready -- logging init happens before main() runs. */
	bool log_ready;

	/** The DW1000 SPI bus and its control GPIOs (reset/int) report
	 *  device_is_ready()/gpio_is_ready_dt() -- a presence/wiring check
	 *  only. Does NOT talk to the DW1000 chip itself (no reset pulse, no
	 *  dwt_initialise(), no register read) -- see the TODO in
	 *  image_health_zephyr.c for where an actual "DW1000 responds" check
	 *  hooks in once tag DW1000 bring-up lands in src/main.c. */
	bool dw1000_bus_ready;
};

/**
 * @brief Health-gate verdict.
 */
enum image_health_result {
	IMAGE_HEALTH_OK = 0,
	IMAGE_HEALTH_FAIL,
};

/**
 * @brief Pure decision function: does this set of checks pass the gate?
 *
 * All checks must be true for the gate to pass. Host-testable (ztest/native,
 * no Zephyr kernel/devicetree needed) -- see tests/image_health.
 *
 * @param checks  Snapshot of the individual checks. NULL fails the gate.
 * @return IMAGE_HEALTH_OK if every check in @p checks passed,
 *         IMAGE_HEALTH_FAIL otherwise.
 */
enum image_health_result image_health_evaluate(const struct image_health_checks *checks);

/**
 * @brief Gather the real boot-time checks and evaluate the gate.
 *
 * Zephyr/hardware-only (image_health_zephyr.c; not compiled into the
 * tests/image_health host-test binary). Populates a
 * struct image_health_checks from actual kernel/devicetree state and
 * delegates the pass/fail decision to image_health_evaluate().
 *
 * Call this early in main(), before any lengthy/blocking work, so a broken
 * image is caught (and therefore never calls boot_write_img_confirmed())
 * as soon as possible after boot.
 *
 * @return The verdict from image_health_evaluate() on the gathered checks.
 */
enum image_health_result image_health_run_checks(void);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_HEALTH_H_ */
