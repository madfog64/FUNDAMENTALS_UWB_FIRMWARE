/*! ----------------------------------------------------------------------------
 * @file    dw1000_ranging.c
 * @brief   DW1000 ranging radio primitives: RX-with-timestamp and scheduled
 *          (delayed) TX (UWB-231).
 *
 * See dw1000_ranging.h for the full API doc. Implementation notes:
 *
 *   dw1000_rx()      dwt_setrxtimeout() + dwt_rxenable(DWT_START_RX_IMMEDIATE),
 *                     then busy-polls SYS_STATUS for RXFCG (good frame) vs
 *                     ALL_RX_ERR / ALL_RX_TO. Mirrors the standard Decawave
 *                     polled-RX example pattern (ex_02a/02b in the DW1000 API
 *                     distribution) but returns errno-style codes instead of
 *                     looping forever on error.
 *
 *   dw1000_tx_at()    dwt_setdelayedtrxtime() + dwt_writetxdata() +
 *                     dwt_writetxfctrl() + dwt_starttx(DWT_START_TX_DELAYED
 *                     [| DWT_RESPONSE_EXPECTED]), then polls SYS_STATUS for
 *                     TXFRS. dwt_starttx() itself reports a missed slot
 *                     (HPDWARN) via its return value — surfaced here as -EIO.
 *
 * Antenna delay is already programmed by dw1000_configure() (UWB-7, UWB-155,
 * UWB-156) before any function in this module runs, so every timestamp
 * handled here is already antenna-delay corrected in hardware (TX_ANTD /
 * LDE_RXANTD).
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/logging/log.h>

#include "deca_device_api.h"
#include "deca_regs.h"
#include "dw1000_ranging.h"

LOG_MODULE_REGISTER(dw1000_ranging, LOG_LEVEL_DBG);

/* ---------------------------------------------------------------------------
 * Conversion / masking constants
 * --------------------------------------------------------------------------- */

/*
 * DW1000 RX frame-wait timeout register (RX_FWTO, programmed via
 * dwt_setrxtimeout()) units: 512/499.2 MHz ~= 1.0256 us per LSB (DW1000 User
 * Manual sec 7.2.40). Converted with integer math (avoids pulling in
 * <math.h> purely for a rounding division):
 *
 *   units = timeout_us * DW1000_RX_TIMEOUT_NUM / DW1000_RX_TIMEOUT_DEN
 *         = timeout_us * 10000 / 10256   (10256 = 10000 * 1.0256)
 */
#define DW1000_RX_TIMEOUT_NUM   10000ULL
#define DW1000_RX_TIMEOUT_DEN   10256ULL

/* DW1000 system time counter is 40 bits wide. */
#define DW1000_TIME_MASK_40BIT  ((uint64_t)0xFFFFFFFFFFULL)

/*
 * The DW1000 ignores the low 9 bits of a programmed delayed TX/RX time
 * (DW1000 User Manual sec 3.3) -- the true schedulable granularity is
 * 2^9 = 512 ticks (~8.01 ns), one bit finer than the 2^8 truncation implied
 * by dwt_setdelayedtrxtime()'s 32-bit (high-bits-only) argument alone.
 */
#define DW1000_DELAYED_TIME_IGNORED_BITS   9u
#define DW1000_DELAYED_TIME_MASK \
    (~(((uint64_t)1u << DW1000_DELAYED_TIME_IGNORED_BITS) - 1u))

/* ---------------------------------------------------------------------------
 * us_to_rx_timeout_units
 * --------------------------------------------------------------------------- */
static uint16_t us_to_rx_timeout_units(uint32_t timeout_us)
{
    uint64_t units;

    if (timeout_us == 0u) {
        /* dwt_setrxtimeout(0) disables the hardware timeout entirely --
         * pass it straight through rather than rounding up to 1. */
        return 0u;
    }

    units = ((uint64_t)timeout_us * DW1000_RX_TIMEOUT_NUM) / DW1000_RX_TIMEOUT_DEN;

    if (units == 0u) {
        /* A non-zero requested timeout must never round down to 0 -- that
         * would silently disable the hardware timeout. */
        units = 1u;
    }
    if (units > 0xFFFFu) {
        units = 0xFFFFu;
    }
    return (uint16_t)units;
}

/* ---------------------------------------------------------------------------
 * 40-bit timestamp assembly
 * --------------------------------------------------------------------------- */
static uint64_t assemble_40bit_timestamp(const uint8_t ts_bytes[5])
{
    uint64_t ts = 0;
    int i;

    /* dwt_read{rx,tx}timestamp() fill a 5-byte buffer little-endian:
     * ts_bytes[0] is the LSB, ts_bytes[4] is the MSB (bits [39:32]). */
    for (i = 4; i >= 0; i--) {
        ts = (ts << 8) | (uint64_t)ts_bytes[i];
    }
    return ts;
}

uint64_t dw1000_read_rx_timestamp(void)
{
    uint8_t ts_bytes[5] = {0};

    dwt_readrxtimestamp(ts_bytes);
    return assemble_40bit_timestamp(ts_bytes);
}

uint64_t dw1000_read_tx_timestamp(void)
{
    uint8_t ts_bytes[5] = {0};

    dwt_readtxtimestamp(ts_bytes);
    return assemble_40bit_timestamp(ts_bytes);
}

/* ---------------------------------------------------------------------------
 * dw1000_rx
 * --------------------------------------------------------------------------- */
int dw1000_rx(uint8_t *buf, uint16_t *len, uint64_t *rx_ts, uint32_t timeout_us)
{
    uint16_t buf_cap;
    uint32 status;
    int ret;

    if (buf == NULL || len == NULL || rx_ts == NULL) {
        return -EINVAL;
    }

    buf_cap = *len;
    *len = 0;

    dwt_setrxtimeout(us_to_rx_timeout_units(timeout_us));

    ret = dwt_rxenable(DWT_START_RX_IMMEDIATE);
    if (ret != DWT_SUCCESS) {
        LOG_ERR("dwt_rxenable() failed (%d)", ret);
        return -EIO;
    }

    /* Busy-poll SYS_STATUS. Bounded by the DW1000 hardware RX timeout
     * programmed above -- this loop always terminates on real hardware. */
    for (;;) {
        status = dwt_read32bitreg(SYS_STATUS_ID);

        if (status & SYS_STATUS_RXFCG) {
            break;
        }
        if (status & SYS_STATUS_ALL_RX_TO) {
            dwt_write32bitreg(SYS_STATUS_ID, (uint32)SYS_STATUS_ALL_RX_TO);
            dwt_forcetrxoff();
            LOG_DBG("dw1000_rx() timed out (status 0x%08X)", (unsigned)status);
            return -ETIMEDOUT;
        }
        if (status & SYS_STATUS_ALL_RX_ERR) {
            dwt_write32bitreg(SYS_STATUS_ID, (uint32)SYS_STATUS_ALL_RX_ERR);
            dwt_forcetrxoff();
            LOG_ERR("dw1000_rx() RX error (status 0x%08X)", (unsigned)status);
            return -EIO;
        }
    }

    /* Good frame: read the length from RX_FINFO, then the payload. */
    {
        uint32 finfo = dwt_read32bitreg(RX_FINFO_ID);
        uint16_t frame_len = (uint16_t)(finfo & RX_FINFO_RXFLEN_MASK);

        if (frame_len > buf_cap) {
            dwt_write32bitreg(SYS_STATUS_ID, (uint32)SYS_STATUS_RXFCG);
            dwt_forcetrxoff();
            LOG_ERR("dw1000_rx() frame len %u exceeds buffer capacity %u",
                    (unsigned)frame_len, (unsigned)buf_cap);
            return -EINVAL;
        }

        dwt_readrxdata(buf, frame_len, 0);
        *len = frame_len;
    }

    *rx_ts = dw1000_read_rx_timestamp();

    /* Clear the good-frame status bit so the next dw1000_rx() call starts
     * from a clean SYS_STATUS. */
    dwt_write32bitreg(SYS_STATUS_ID, (uint32)SYS_STATUS_RXFCG);

    return 0;
}

/* ---------------------------------------------------------------------------
 * dw1000_delayed_tx_time
 * --------------------------------------------------------------------------- */
uint64_t dw1000_delayed_tx_time(uint64_t anchor_ts, uint32_t delay_dtu)
{
    uint64_t raw = (anchor_ts + (uint64_t)delay_dtu) & DW1000_TIME_MASK_40BIT;

    return raw & DW1000_DELAYED_TIME_MASK & DW1000_TIME_MASK_40BIT;
}

/* ---------------------------------------------------------------------------
 * dw1000_tx_at
 * --------------------------------------------------------------------------- */
int dw1000_tx_at(const uint8_t *buf, uint16_t len, uint64_t tx_time_dtu,
                  bool expect_response)
{
    uint32 status;
    uint8 mode;
    int ret;

    if (buf == NULL || len == 0u) {
        return -EINVAL;
    }

    /* dwt_setdelayedtrxtime() takes only the HIGH 32 bits of the 40-bit
     * DX_TIME register (bits [39:8]) -- see the dw1000_tx_at() doc comment
     * in dw1000_ranging.h for the antenna-delay / granularity interaction. */
    dwt_setdelayedtrxtime((uint32)(tx_time_dtu >> 8));

    ret = dwt_writetxdata(len, (uint8 *)buf, 0);
    if (ret != DWT_SUCCESS) {
        LOG_ERR("dwt_writetxdata() rejected frame (len=%u)", (unsigned)len);
        return -EINVAL;
    }

    /* ranging = 1: every frame this module transmits is a TWR/TDoA ranging
     * frame (blink, poll, response, final, sync). */
    dwt_writetxfctrl(len, 0, 1);

    mode = (uint8)DWT_START_TX_DELAYED;
    if (expect_response) {
        mode |= (uint8)DWT_RESPONSE_EXPECTED;
    }

    ret = dwt_starttx(mode);
    if (ret != DWT_SUCCESS) {
        /* dwt_starttx() reports DWT_ERROR when the scheduled slot was
         * missed (HPDWARN latched in SYS_STATUS because tx_time_dtu had
         * already passed) -- abort rather than transmit at an unplanned
         * time the peer isn't expecting. */
        status = dwt_read32bitreg(SYS_STATUS_ID);
        dwt_write32bitreg(SYS_STATUS_ID, (uint32)SYS_STATUS_HPDWARN);
        dwt_forcetrxoff();
        LOG_ERR("dw1000_tx_at() missed scheduled slot (status 0x%08X)",
                (unsigned)status);
        return -EIO;
    }

    /* Wait for the transmission to complete before returning. */
    do {
        status = dwt_read32bitreg(SYS_STATUS_ID);
    } while (!(status & SYS_STATUS_TXFRS));

    dwt_write32bitreg(SYS_STATUS_ID, (uint32)SYS_STATUS_TXFRS);

    return 0;
}
