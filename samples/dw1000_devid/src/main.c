/*! ----------------------------------------------------------------------------
 * @file    main.c
 * @brief   DW1000 device-ID bring-up sample (UWB-94)
 *
 * Reads the DW1000 DEV_ID register after reset and logs PASS when it equals
 * the expected value (0xDECA0130).  This is the minimal bring-up verification
 * before any UWB TX/RX is configured.
 *
 * Sequence:
 *   1. dw1000_port_init()       — configure SPI, GPIOs, work queue item
 *   2. reset_DW1000()           — assert then release hardware reset
 *   3. port_set_dw1000_slowrate() — set SPI < 3 MHz (DW1000 requirement)
 *   4. dwt_initialise()         — verify device ID internally, load ucode
 *   5. port_set_dw1000_fastrate() — raise SPI to 8 MHz
 *   6. dwt_readdevid()          — read DEV_ID register (0x00), 4 bytes
 *   7. Log PASS / FAIL
 *
 * NOTE: This sample requires an on-target run with a real DWM1001 module.
 *       For headless CI verification see tests/dw1000_devid/ (ztest, unit_testing).
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "deca_device_api.h"
#include "deca_port.h"

LOG_MODULE_REGISTER(dw1000_devid, LOG_LEVEL_DBG);

int main(void)
{
	int ret;
	uint32 devid;

	LOG_INF("DW1000 device-ID bring-up sample starting");

	/* 1. Initialise port layer (SPI config, GPIOs, deferred ISR work item). */
	ret = dw1000_port_init();
	if (ret < 0) {
		LOG_ERR("dw1000_port_init failed: %d", ret);
		return ret;
	}

	/* 2. Hardware reset — pulse RESET low then release. */
	reset_DW1000();

	/* 3. Switch to slow SPI rate (< 3 MHz) before dwt_initialise(). */
	port_set_dw1000_slowrate();

	/* 4. Initialise the DW1000 driver.
	 *    DWT_LOADNONE: skip LDE microcode upload — sufficient for devid read.
	 *    This call internally reads DEV_ID and returns DWT_ERROR if it does not
	 *    match 0xDECA0130 (any NRFX SPI misconfiguration surfaces here). */
	ret = dwt_initialise(DWT_LOADNONE);
	if (ret != DWT_SUCCESS) {
		LOG_ERR("dwt_initialise failed (%d) — check SPI wiring and slow rate", ret);
		return -EIO;
	}

	/* 5. Raise SPI to fast rate now that initialisation is complete. */
	port_set_dw1000_fastrate();

	/* 6. Read and verify the device ID directly. */
	devid = dwt_readdevid();

	if (devid == DWT_DEVICE_ID) {
		LOG_INF("PASS: DW1000 DEV_ID = 0x%08X (expected 0x%08X)",
			devid, DWT_DEVICE_ID);
	} else {
		LOG_ERR("FAIL: DW1000 DEV_ID = 0x%08X (expected 0x%08X)",
			devid, DWT_DEVICE_ID);
	}

	/* Idle — the sample has no further work to do. */
	while (1) {
		k_sleep(K_MSEC(5000));
		LOG_DBG("Heartbeat — DEV_ID last read: 0x%08X", devid);
	}

	return 0;
}
