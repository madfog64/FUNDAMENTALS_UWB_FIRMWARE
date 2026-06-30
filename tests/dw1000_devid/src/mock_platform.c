/*! ----------------------------------------------------------------------------
 * @file    mock_platform.c
 * @brief   Mock implementations of the five deca_driver platform symbols.
 *
 * These replace deca_port.c in the unit_testing build.  They have no Zephyr
 * SPI/GPIO dependencies — the mock captures the SPI header that the driver
 * constructs for the DEV_ID read and fills the RX buffer with the expected
 * 0xDECA0130 device-ID bytes.
 *
 * The captured state is in mock_spi_state (declared in mock_platform.h) and
 * is reset by mock_spi_reset() before each test.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <string.h>

#include "deca_device_api.h"
#include "mock_platform.h"

/* ---------------------------------------------------------------------------
 * Captured SPI call state (inspected by the test assertions)
 * --------------------------------------------------------------------------- */
struct mock_spi_state mock_spi_state;

void mock_spi_reset(void)
{
	memset(&mock_spi_state, 0, sizeof(mock_spi_state));
}

/* ---------------------------------------------------------------------------
 * readfromspi — the key mock
 *
 * Captures the header the driver builds for the SPI read transaction (this is
 * what the framing test asserts), then fills readBuffer with the DEV_ID bytes
 * for 0xDECA0130 in little-endian order so that dwt_read32bitoffsetreg()
 * decodes the correct value.
 *
 * DW1000 registers are read little-endian on the wire:
 *   0xDECA0130 -> buffer[0]=0x30, [1]=0x01, [2]=0xCA, [3]=0xDE
 * --------------------------------------------------------------------------- */
int readfromspi(uint16 headerLength, const uint8 *headerBuffer,
		uint32 readlength, uint8 *readBuffer)
{
	/* Capture the header so the test can verify the framing. */
	mock_spi_state.read_header_len = headerLength;
	if (headerLength <= MOCK_SPI_MAX_HDR) {
		memcpy(mock_spi_state.read_header, headerBuffer, headerLength);
	}
	mock_spi_state.read_called = 1;

	/*
	 * Fill readBuffer with 0xDECA0130 in little-endian byte order.
	 * Any read of less than 4 bytes receives the appropriate prefix bytes.
	 */
	static const uint8 devid_le[4] = { 0x30U, 0x01U, 0xCAU, 0xDEU };
	uint32 to_fill = readlength < 4U ? readlength : 4U;
	uint32 i;

	for (i = 0U; i < to_fill; i++) {
		readBuffer[i] = devid_le[i];
	}
	/* Zero any remaining bytes beyond the devid pattern. */
	for (; i < readlength; i++) {
		readBuffer[i] = 0U;
	}

	return DWT_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * writetospi — no-op stub (not exercised by dwt_readdevid)
 * --------------------------------------------------------------------------- */
int writetospi(uint16 headerLength, const uint8 *headerBuffer,
	       uint32 bodylength, const uint8 *bodyBuffer)
{
	(void)headerLength;
	(void)headerBuffer;
	(void)bodylength;
	(void)bodyBuffer;
	return DWT_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * decamutexon / decamutexoff — no-op stubs (single-threaded test)
 * --------------------------------------------------------------------------- */
decaIrqStatus_t decamutexon(void)
{
	return 0;
}

void decamutexoff(decaIrqStatus_t s)
{
	(void)s;
}

/* ---------------------------------------------------------------------------
 * deca_sleep — no-op stub (no k_msleep available on unit_testing)
 * --------------------------------------------------------------------------- */
void deca_sleep(unsigned int time_ms)
{
	(void)time_ms;
}
