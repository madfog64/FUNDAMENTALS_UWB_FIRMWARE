/*! ----------------------------------------------------------------------------
 * @file    test_dw1000_ranging.c
 * @brief   ztest suite: DW1000 ranging radio primitives (UWB-231)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI).
 *
 * Covers:
 *   dw1000_tx_at()             delayed-time programming, frame write, mode
 *                               bits (plain vs DWT_RESPONSE_EXPECTED), and
 *                               the HPDWARN (missed slot) error path.
 *   dw1000_rx()                good-frame / RX-error / RX-timeout paths,
 *                               and the oversized-frame guard.
 *   dw1000_read_rx_timestamp() /
 *   dw1000_read_tx_timestamp() little-endian 40-bit assembly.
 *   dw1000_delayed_tx_time()   low-9-bit truncation, including 40-bit
 *                               wraparound.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "deca_device_api.h"
#include "deca_regs.h"
#include "dw1000_ranging.h"
#include "mock_deca_driver.h"

/* ---------------------------------------------------------------------------
 * Suite setup / teardown
 * --------------------------------------------------------------------------- */
static void before_each(void *fixture)
{
    (void)fixture;
    mock_deca_reset();
}

ZTEST_SUITE(dw1000_ranging, NULL, NULL, before_each, NULL, NULL);

/* =============================================================================
 * dw1000_tx_at()
 * ========================================================================= */

/* ---------------------------------------------------------------------------
 * Test — delayed time is programmed, frame is written, plain delayed mode
 *
 * dwt_setdelayedtrxtime() must receive the HIGH 32 bits of tx_time_dtu
 * (tx_time_dtu >> 8). dwt_writetxdata()/dwt_writetxfctrl() must receive the
 * exact frame bytes/length, with the ranging bit set. dwt_starttx() must be
 * called with DWT_START_TX_DELAYED only (no DWT_RESPONSE_EXPECTED) when
 * expect_response is false.
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_ranging, test_tx_at_programs_delayed_time_and_frame)
{
    const uint8_t frame[] = {0xDE, 0xAD, 0xBE, 0xEF};
    const uint64_t tx_time_dtu = 0x0123456789ULL;

    /* Let the post-starttx TXFRS poll succeed immediately. */
    mock_reg_state.sys_status = SYS_STATUS_TXFRS;

    int ret = dw1000_tx_at(frame, sizeof(frame), tx_time_dtu, false);

    zassert_equal(ret, 0, "dw1000_tx_at() returned %d (expected 0)", ret);

    zassert_equal(mock_delayedtrxtime_state.called, 1,
        "Expected dwt_setdelayedtrxtime() called once; got %d",
        mock_delayedtrxtime_state.called);
    zassert_equal(mock_delayedtrxtime_state.starttime,
        (uint32)(tx_time_dtu >> 8),
        "dwt_setdelayedtrxtime(): expected high 32 bits 0x%08X; got 0x%08X",
        (unsigned)(tx_time_dtu >> 8), (unsigned)mock_delayedtrxtime_state.starttime);

    zassert_equal(mock_tx_state.writetxdata_called, 1,
        "Expected dwt_writetxdata() called once; got %d",
        mock_tx_state.writetxdata_called);
    zassert_equal(mock_tx_state.writetxdata_len, (uint16)sizeof(frame),
        "writetxdata length: expected %u; got %u",
        (unsigned)sizeof(frame), (unsigned)mock_tx_state.writetxdata_len);
    zassert_equal(mock_tx_state.writetxdata_offset, 0,
        "writetxdata offset: expected 0; got %u",
        (unsigned)mock_tx_state.writetxdata_offset);
    zassert_mem_equal(mock_tx_state.writetxdata_buf, frame, sizeof(frame),
        "writetxdata: frame bytes do not match");

    zassert_equal(mock_tx_state.writetxfctrl_called, 1,
        "Expected dwt_writetxfctrl() called once; got %d",
        mock_tx_state.writetxfctrl_called);
    zassert_equal(mock_tx_state.writetxfctrl_len, (uint16)sizeof(frame),
        "writetxfctrl length: expected %u; got %u",
        (unsigned)sizeof(frame), (unsigned)mock_tx_state.writetxfctrl_len);
    zassert_equal(mock_tx_state.writetxfctrl_ranging, 1,
        "writetxfctrl: expected ranging=1; got %d",
        mock_tx_state.writetxfctrl_ranging);

    zassert_equal(mock_starttx_state.called, 1,
        "Expected dwt_starttx() called once; got %d", mock_starttx_state.called);
    zassert_equal(mock_starttx_state.mode, (uint8)DWT_START_TX_DELAYED,
        "dwt_starttx() mode: expected DWT_START_TX_DELAYED (0x%02X) only; "
        "got 0x%02X",
        (unsigned)DWT_START_TX_DELAYED, (unsigned)mock_starttx_state.mode);
}

/* ---------------------------------------------------------------------------
 * Test — expect_response=true also sets DWT_RESPONSE_EXPECTED
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_ranging, test_tx_at_expect_response_sets_response_bit)
{
    const uint8_t frame[] = {0x01, 0x02};

    mock_reg_state.sys_status = SYS_STATUS_TXFRS;

    int ret = dw1000_tx_at(frame, sizeof(frame), 0x1000ULL, true);

    zassert_equal(ret, 0, "dw1000_tx_at() returned %d (expected 0)", ret);

    const uint8_t expected_mode =
        (uint8_t)(DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);

    zassert_equal(mock_starttx_state.mode, expected_mode,
        "dwt_starttx() mode: expected DELAYED|RESPONSE_EXPECTED (0x%02X); "
        "got 0x%02X", (unsigned)expected_mode, (unsigned)mock_starttx_state.mode);
}

/* ---------------------------------------------------------------------------
 * Test — a missed slot (HPDWARN) surfaces as -EIO
 *
 * When dwt_starttx() returns DWT_ERROR (the DW1000 latched HPDWARN because
 * the scheduled time had already passed), dw1000_tx_at() must return -EIO
 * and must NOT wait on TXFRS (no transmission occurred).
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_ranging, test_tx_at_hpdwarn_returns_eio)
{
    const uint8_t frame[] = {0xAA};

    mock_starttx_state.return_value = DWT_ERROR;
    /* Deliberately leave sys_status without TXFRS set -- if dw1000_tx_at()
     * incorrectly proceeded to the TXFRS wait loop after an error, this test
     * would hang, which twister will report as a timeout/failure. */

    int ret = dw1000_tx_at(frame, sizeof(frame), 0x1000ULL, false);

    zassert_equal(ret, -EIO,
        "Expected dw1000_tx_at() to return -EIO on HPDWARN; got %d", ret);
    zassert_equal(mock_starttx_state.called, 1,
        "Expected dwt_starttx() to be called once; got %d",
        mock_starttx_state.called);
    zassert_equal(mock_forcetrxoff_state.called, 1,
        "Expected dwt_forcetrxoff() called once on HPDWARN; got %d",
        mock_forcetrxoff_state.called);
}

/* ---------------------------------------------------------------------------
 * Test — NULL buffer / zero length rejected before touching the driver
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_ranging, test_tx_at_rejects_null_buf_and_zero_len)
{
    const uint8_t frame[] = {0xAA};

    zassert_equal(dw1000_tx_at(NULL, 1, 0x1000ULL, false), -EINVAL,
        "Expected -EINVAL for NULL buf");
    zassert_equal(dw1000_tx_at(frame, 0, 0x1000ULL, false), -EINVAL,
        "Expected -EINVAL for zero len");
    zassert_equal(mock_delayedtrxtime_state.called, 0,
        "dwt_setdelayedtrxtime() must not be called on an invalid argument");
}

/* =============================================================================
 * dw1000_rx()
 * ========================================================================= */

/* ---------------------------------------------------------------------------
 * Test — a good frame returns the injected bytes and assembled RX timestamp
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_ranging, test_rx_good_frame_returns_bytes_and_timestamp)
{
    const uint8_t injected[] = {0x10, 0x11, 0x12, 0x13, 0x14};
    uint8_t buf[32] = {0};
    uint16_t len = sizeof(buf);
    uint64_t rx_ts = 0;

    mock_reg_state.sys_status = SYS_STATUS_RXFCG;
    mock_reg_state.rx_finfo   = (uint32)sizeof(injected);
    memcpy(mock_readrxdata_state.injected_payload, injected, sizeof(injected));

    /* rx_ts bytes little-endian: byte[0] = LSB ... byte[4] = MSB. */
    mock_timestamp_state.rx_ts_bytes[0] = 0x11;
    mock_timestamp_state.rx_ts_bytes[1] = 0x22;
    mock_timestamp_state.rx_ts_bytes[2] = 0x33;
    mock_timestamp_state.rx_ts_bytes[3] = 0x44;
    mock_timestamp_state.rx_ts_bytes[4] = 0x05;
    const uint64_t expected_rx_ts = 0x0544332211ULL;

    int ret = dw1000_rx(buf, &len, &rx_ts, 1000);

    zassert_equal(ret, 0, "dw1000_rx() returned %d (expected 0)", ret);
    zassert_equal(len, (uint16_t)sizeof(injected),
        "len: expected %u; got %u", (unsigned)sizeof(injected), (unsigned)len);
    zassert_mem_equal(buf, injected, sizeof(injected),
        "dw1000_rx(): received payload does not match injected bytes");
    zassert_equal(rx_ts, expected_rx_ts,
        "rx_ts: expected 0x%010llX; got 0x%010llX",
        (unsigned long long)expected_rx_ts, (unsigned long long)rx_ts);

    zassert_equal(mock_rxtimeout_state.called, 1,
        "Expected dwt_setrxtimeout() called once; got %d",
        mock_rxtimeout_state.called);
    zassert_equal(mock_rxenable_state.called, 1,
        "Expected dwt_rxenable() called once; got %d",
        mock_rxenable_state.called);
    zassert_equal(mock_rxenable_state.mode, DWT_START_RX_IMMEDIATE,
        "dwt_rxenable() mode: expected DWT_START_RX_IMMEDIATE (%d); got %d",
        DWT_START_RX_IMMEDIATE, mock_rxenable_state.mode);

    zassert_equal(mock_readrxdata_state.called, 1,
        "Expected dwt_readrxdata() called once; got %d",
        mock_readrxdata_state.called);
    zassert_equal(mock_readrxdata_state.length, (uint16)sizeof(injected),
        "dwt_readrxdata() length: expected %u; got %u",
        (unsigned)sizeof(injected), (unsigned)mock_readrxdata_state.length);
    zassert_equal(mock_readrxdata_state.offset, 0,
        "dwt_readrxdata() offset: expected 0; got %u",
        (unsigned)mock_readrxdata_state.offset);
}

/* ---------------------------------------------------------------------------
 * Test — an RX timeout event (SYS_STATUS_ALL_RX_TO) returns -ETIMEDOUT
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_ranging, test_rx_timeout_returns_etimedout)
{
    uint8_t buf[16];
    uint16_t len = sizeof(buf);
    uint64_t rx_ts = 0;

    /* SYS_STATUS_RXRFTO (Receive Frame Wait Timeout) is part of
     * SYS_STATUS_ALL_RX_TO. */
    mock_reg_state.sys_status = SYS_STATUS_RXRFTO;

    int ret = dw1000_rx(buf, &len, &rx_ts, 1000);

    zassert_equal(ret, -ETIMEDOUT,
        "Expected -ETIMEDOUT on RX timeout; got %d", ret);
    zassert_equal(len, 0, "len must be reset to 0 on error; got %u", (unsigned)len);
    zassert_equal(mock_forcetrxoff_state.called, 1,
        "Expected dwt_forcetrxoff() called once on timeout; got %d",
        mock_forcetrxoff_state.called);
}

/* ---------------------------------------------------------------------------
 * Test — an RX error event (SYS_STATUS_ALL_RX_ERR) returns -EIO
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_ranging, test_rx_error_returns_eio)
{
    uint8_t buf[16];
    uint16_t len = sizeof(buf);
    uint64_t rx_ts = 0;

    /* SYS_STATUS_RXFCE (Receiver FCS Error) is part of SYS_STATUS_ALL_RX_ERR. */
    mock_reg_state.sys_status = SYS_STATUS_RXFCE;

    int ret = dw1000_rx(buf, &len, &rx_ts, 1000);

    zassert_equal(ret, -EIO, "Expected -EIO on RX error; got %d", ret);
    zassert_equal(len, 0, "len must be reset to 0 on error; got %u", (unsigned)len);
    zassert_equal(mock_forcetrxoff_state.called, 1,
        "Expected dwt_forcetrxoff() called once on RX error; got %d",
        mock_forcetrxoff_state.called);
}

/* ---------------------------------------------------------------------------
 * Test — a frame larger than the caller's buffer capacity is rejected
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_ranging, test_rx_frame_too_large_returns_einval)
{
    uint8_t buf[8];
    uint16_t len = sizeof(buf); /* capacity = 8 */
    uint64_t rx_ts = 0;

    mock_reg_state.sys_status = SYS_STATUS_RXFCG;
    mock_reg_state.rx_finfo   = 50u; /* frame claims 50 bytes -- exceeds capacity */

    int ret = dw1000_rx(buf, &len, &rx_ts, 1000);

    zassert_equal(ret, -EINVAL,
        "Expected -EINVAL when frame length exceeds buffer capacity; got %d", ret);
    zassert_equal(mock_readrxdata_state.called, 0,
        "dwt_readrxdata() must not be called when the frame doesn't fit");
    zassert_equal(mock_forcetrxoff_state.called, 1,
        "Expected dwt_forcetrxoff() called once on oversize-frame guard; got %d",
        mock_forcetrxoff_state.called);
}

/* ---------------------------------------------------------------------------
 * Test — NULL arguments rejected up front
 * --------------------------------------------------------------------------- */
ZTEST(dw1000_ranging, test_rx_rejects_null_args)
{
    uint8_t buf[8];
    uint16_t len = sizeof(buf);
    uint64_t rx_ts = 0;

    zassert_equal(dw1000_rx(NULL, &len, &rx_ts, 1000), -EINVAL, "NULL buf");
    zassert_equal(dw1000_rx(buf, NULL, &rx_ts, 1000), -EINVAL, "NULL len");
    zassert_equal(dw1000_rx(buf, &len, NULL, 1000), -EINVAL, "NULL rx_ts");
    zassert_equal(mock_rxenable_state.called, 0,
        "dwt_rxenable() must not be called on an invalid argument");
}

/* =============================================================================
 * 40-bit timestamp helpers
 * ========================================================================= */

ZTEST(dw1000_ranging, test_read_rx_timestamp_assembles_little_endian)
{
    mock_timestamp_state.rx_ts_bytes[0] = 0x01;
    mock_timestamp_state.rx_ts_bytes[1] = 0x02;
    mock_timestamp_state.rx_ts_bytes[2] = 0x03;
    mock_timestamp_state.rx_ts_bytes[3] = 0x04;
    mock_timestamp_state.rx_ts_bytes[4] = 0x05;

    uint64_t ts = dw1000_read_rx_timestamp();

    zassert_equal(ts, 0x0504030201ULL,
        "Expected 0x0504030201 (little-endian assembly); got 0x%010llX",
        (unsigned long long)ts);
    zassert_equal(mock_timestamp_state.rx_called, 1,
        "Expected dwt_readrxtimestamp() called once; got %d",
        mock_timestamp_state.rx_called);
}

ZTEST(dw1000_ranging, test_read_tx_timestamp_assembles_little_endian)
{
    mock_timestamp_state.tx_ts_bytes[0] = 0xAA;
    mock_timestamp_state.tx_ts_bytes[1] = 0xBB;
    mock_timestamp_state.tx_ts_bytes[2] = 0xCC;
    mock_timestamp_state.tx_ts_bytes[3] = 0xDD;
    mock_timestamp_state.tx_ts_bytes[4] = 0x0E;

    uint64_t ts = dw1000_read_tx_timestamp();

    zassert_equal(ts, 0x0EDDCCBBAAULL,
        "Expected 0x0EDDCCBBAA (little-endian assembly); got 0x%010llX",
        (unsigned long long)ts);
    zassert_equal(mock_timestamp_state.tx_called, 1,
        "Expected dwt_readtxtimestamp() called once; got %d",
        mock_timestamp_state.tx_called);
}

/* =============================================================================
 * dw1000_delayed_tx_time() -- low-9-bit truncation golden values
 * ========================================================================= */

ZTEST(dw1000_ranging, test_delayed_tx_time_truncates_low_9_bits)
{
    /* 0 + 0x2FF (767) = 767 -> masked to the nearest lower multiple of 512
     * (2^9): 767 & ~0x1FF = 0x200 (512). */
    zassert_equal(dw1000_delayed_tx_time(0, 0x2FFu), 0x200ULL,
        "anchor=0 delay=0x2FF: expected 0x200");

    /* 100 + 1000 = 1100 -> 1100 & ~0x1FF = 1024 (0x400). */
    zassert_equal(dw1000_delayed_tx_time(100, 1000u), 0x400ULL,
        "anchor=100 delay=1000: expected 0x400");

    /* Exact multiple of 512 is left unchanged. */
    zassert_equal(dw1000_delayed_tx_time(0, 1024u), 1024ULL,
        "anchor=0 delay=1024 (exact multiple of 512): expected unchanged");
}

ZTEST(dw1000_ranging, test_delayed_tx_time_wraps_at_40_bits)
{
    /* anchor_ts = 2^40 - 100; + delay 200 wraps to 100 (mod 2^40), which is
     * then truncated to the nearest lower multiple of 512: 100 & ~0x1FF = 0. */
    const uint64_t anchor_ts = (((uint64_t)1u) << 40) - 100u;

    zassert_equal(dw1000_delayed_tx_time(anchor_ts, 200u), 0ULL,
        "40-bit wraparound: anchor=2^40-100 delay=200 -> raw=100 -> expected 0");
}
