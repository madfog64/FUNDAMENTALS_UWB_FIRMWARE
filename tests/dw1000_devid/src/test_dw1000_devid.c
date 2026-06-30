/*! ----------------------------------------------------------------------------
 * @file    test_dw1000_devid.c
 * @brief   ztest suite: DW1000 device-ID SPI framing and decode (UWB-94)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI).
 *
 * Two tests in the suite:
 *
 *   test_readfromspi_header_framing
 *       Verifies that dwt_readdevid() causes the deca_driver to call
 *       readfromspi() with the correct SPI header for the DEV_ID register:
 *         - headerLength == 1  (no sub-index; DEV_ID reg is at index 0)
 *         - header[0]    == 0x00  (READ | no-subaddr | reg file id 0x00)
 *       This tests the header encoding in dwt_readfromdevice().
 *
 *   test_devid_decode
 *       Verifies that when readfromspi() returns the correct little-endian
 *       bytes {0x30, 0x01, 0xCA, 0xDE}, dwt_readdevid() assembles them into
 *       0xDECA0130 and returns that value.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <zephyr/ztest.h>

#include "deca_device_api.h"
#include "mock_platform.h"

/* ---------------------------------------------------------------------------
 * Suite setup / teardown
 * --------------------------------------------------------------------------- */
static void *suite_setup(void)
{
	/* Nothing to allocate. */
	return NULL;
}

static void before_each(void *fixture)
{
	(void)fixture;
	mock_spi_reset();
}

ZTEST_SUITE(dw1000_devid, NULL, suite_setup, before_each, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Test 1 — readfromspi header framing
 *
 * dwt_readdevid() -> dwt_read32bitoffsetreg(DEV_ID_ID=0, 0)
 *                 -> dwt_readfromdevice(recordNumber=0, index=0, length=4, buf)
 *                 -> readfromspi(cnt=1, header={0x00}, 4, buf)
 *
 * The header encoding (from deca_device.c dwt_readfromdevice):
 *   index == 0  =>  header[0] = (uint8) recordNumber = 0x00
 *                   (bit-7 = 0: READ op; bit-6 = 0: no sub-address; bits 5-0 = 0)
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_devid, test_readfromspi_header_framing)
{
	(void)dwt_readdevid();

	zassert_equal(mock_spi_state.read_called, 1,
		      "Expected readfromspi() to be called exactly once");

	zassert_equal(mock_spi_state.read_header_len, 1,
		      "Expected 1-byte SPI header for DEV_ID (reg 0x00, index 0); "
		      "got %u", mock_spi_state.read_header_len);

	zassert_equal(mock_spi_state.read_header[0], 0x00U,
		      "Expected header byte 0x00 (READ | no-subaddr | reg-id=0); "
		      "got 0x%02X", mock_spi_state.read_header[0]);
}

/* ---------------------------------------------------------------------------
 * Test 2 — dwt_readdevid() value decode
 *
 * The mock fills readBuffer with {0x30, 0x01, 0xCA, 0xDE} (LE byte order for
 * 0xDECA0130).  dwt_read32bitoffsetreg() assembles them as:
 *   buf[0] | (buf[1]<<8) | (buf[2]<<16) | (buf[3]<<24) == 0xDECA0130
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_devid, test_devid_decode)
{
	uint32 devid = dwt_readdevid();

	zassert_equal(devid, DWT_DEVICE_ID,
		      "Expected dwt_readdevid() == 0x%08X (DWT_DEVICE_ID); "
		      "got 0x%08X", DWT_DEVICE_ID, devid);
}
