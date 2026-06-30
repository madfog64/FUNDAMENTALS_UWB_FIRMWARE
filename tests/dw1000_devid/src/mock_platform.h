/*! ----------------------------------------------------------------------------
 * @file    mock_platform.h
 * @brief   Shared state for the readfromspi mock (used by the ztest assertions).
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef MOCK_PLATFORM_H_
#define MOCK_PLATFORM_H_

#include <stdint.h>
#include "deca_device_api.h"

#define MOCK_SPI_MAX_HDR 3U

/** Captured state from the most recent readfromspi() call. */
struct mock_spi_state {
	uint16 read_header_len;          /**< Header length passed to readfromspi. */
	uint8  read_header[MOCK_SPI_MAX_HDR]; /**< Header bytes passed to readfromspi. */
	int    read_called;              /**< Non-zero if readfromspi was called. */
};

/** Exposed for inspection by the ztest assertions. */
extern struct mock_spi_state mock_spi_state;

/** Reset captured state before each test. */
void mock_spi_reset(void);

#endif /* MOCK_PLATFORM_H_ */
