/*! ----------------------------------------------------------------------------
 * @file    test_dw1000_sync.c
 * @brief   ztest suite: tag-side SYNC-frame RX + validate/parse (UWB-242)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI).
 *
 * Covers:
 *   uwb_sync_parse()   Radio-free validate/decode: golden-value LE decode of
 *                        a well-formed SYNC frame, and each individual
 *                        validation failure (wrong length / frame type / PAN
 *                        ID / destination address), plus NULL-argument
 *                        rejection.
 *   dw1000_sync_rx()    Composition over the mocked dw1000_rx(): good SYNC
 *                        frame -> 0 with rx_ts/cycle_seq/master_tx_ts
 *                        populated; wrong frame heard -> -EBADMSG; mock
 *                        timeout -> -ETIMEDOUT (passthrough); mock RX/FCS
 *                        error -> -EIO (passthrough); NULL out -> -EINVAL.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "deca_device_api.h"
#include "deca_regs.h"
#include "dw1000_sync.h"
#include "mock_deca_driver.h"
#include "uwb_frames.h"

/* ---------------------------------------------------------------------------
 * Suite setup / teardown
 * --------------------------------------------------------------------------- */
static void before_each(void *fixture)
{
    (void)fixture;
    mock_deca_reset();
}

ZTEST_SUITE(dw1000_sync, NULL, NULL, before_each, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Test frame builder
 *
 * Hand-packs a raw SYNC frame buffer byte-by-byte from the UWB_OFF_SYNC_*
 * offsets in uwb_frames.h -- deliberately independent of dw1000_sync.c's own
 * encode/decode logic so the test does not become tautological.
 *
 * @p buf must have capacity for at least UWB_SYNC_FRAME_SIZE bytes.
 * --------------------------------------------------------------------------- */
static void build_sync_frame(uint8_t *buf, uint16_t pan_id, uint16_t dest_addr,
                              uint8_t frame_type, uint16_t cycle_seq,
                              uint64_t master_tx_ts)
{
    int i;

    memset(buf, 0, UWB_SYNC_FRAME_SIZE);

    buf[UWB_OFF_FRAME_CTRL]     = UWB_FRAME_CTRL_LOW;
    buf[UWB_OFF_FRAME_CTRL + 1] = UWB_FRAME_CTRL_HIGH;
    buf[UWB_OFF_SEQ_NUM]        = 0x07u;

    buf[UWB_OFF_PAN_ID]     = (uint8_t)(pan_id & 0xFFu);
    buf[UWB_OFF_PAN_ID + 1] = (uint8_t)((pan_id >> 8) & 0xFFu);

    buf[UWB_OFF_DEST_ADDR]     = (uint8_t)(dest_addr & 0xFFu);
    buf[UWB_OFF_DEST_ADDR + 1] = (uint8_t)((dest_addr >> 8) & 0xFFu);

    /* Master's short address -- arbitrary, not validated by uwb_sync_parse(). */
    buf[UWB_OFF_SRC_ADDR]     = 0x01u;
    buf[UWB_OFF_SRC_ADDR + 1] = 0x00u;

    buf[UWB_OFF_FRAME_TYPE] = frame_type;

    buf[UWB_OFF_SYNC_CYCLE_SEQ]     = (uint8_t)(cycle_seq & 0xFFu);
    buf[UWB_OFF_SYNC_CYCLE_SEQ + 1] = (uint8_t)((cycle_seq >> 8) & 0xFFu);

    for (i = 0; i < 5; i++) {
        buf[UWB_OFF_SYNC_MASTER_TX_TS + i] =
            (uint8_t)((master_tx_ts >> (8 * i)) & 0xFFu);
    }
}

/* =============================================================================
 * uwb_sync_parse() -- radio-free validate/decode
 * ========================================================================= */

ZTEST(dw1000_sync, test_parse_valid_sync_decodes_golden_values)
{
    uint8_t buf[UWB_SYNC_FRAME_SIZE];
    uint16_t cycle_seq = 0;
    uint64_t master_tx_ts = 0;
    const uint16_t golden_cycle_seq = 0x1234u;
    const uint64_t golden_master_tx_ts = 0x0102030405ULL;

    build_sync_frame(buf, UWB_PAN_ID, UWB_ADDR_BROADCAST, UWB_FRAME_TYPE_SYNC,
                      golden_cycle_seq, golden_master_tx_ts);

    int ret = uwb_sync_parse(buf, sizeof(buf), &cycle_seq, &master_tx_ts);

    zassert_equal(ret, 0, "uwb_sync_parse() returned %d (expected 0)", ret);
    zassert_equal(cycle_seq, golden_cycle_seq,
        "cycle_seq: expected 0x%04X; got 0x%04X",
        golden_cycle_seq, cycle_seq);
    zassert_equal(master_tx_ts, golden_master_tx_ts,
        "master_tx_ts: expected 0x%010llX; got 0x%010llX",
        (unsigned long long)golden_master_tx_ts,
        (unsigned long long)master_tx_ts);
}

ZTEST(dw1000_sync, test_parse_wrong_length_returns_ebadmsg)
{
    uint8_t buf[UWB_SYNC_FRAME_SIZE];
    uint16_t cycle_seq = 0;
    uint64_t master_tx_ts = 0;

    build_sync_frame(buf, UWB_PAN_ID, UWB_ADDR_BROADCAST, UWB_FRAME_TYPE_SYNC,
                      0x0001u, 0x01ULL);

    int ret = uwb_sync_parse(buf, (uint16_t)(UWB_SYNC_FRAME_SIZE - 1u),
                              &cycle_seq, &master_tx_ts);

    zassert_equal(ret, -EBADMSG,
        "Expected -EBADMSG for wrong length; got %d", ret);
}

ZTEST(dw1000_sync, test_parse_wrong_frame_type_returns_ebadmsg)
{
    uint8_t buf[UWB_SYNC_FRAME_SIZE];
    uint16_t cycle_seq = 0;
    uint64_t master_tx_ts = 0;

    build_sync_frame(buf, UWB_PAN_ID, UWB_ADDR_BROADCAST,
                      UWB_FRAME_TYPE_TAG_BLINK, 0x0001u, 0x01ULL);

    int ret = uwb_sync_parse(buf, sizeof(buf), &cycle_seq, &master_tx_ts);

    zassert_equal(ret, -EBADMSG,
        "Expected -EBADMSG for wrong frame type; got %d", ret);
}

ZTEST(dw1000_sync, test_parse_wrong_pan_returns_ebadmsg)
{
    uint8_t buf[UWB_SYNC_FRAME_SIZE];
    uint16_t cycle_seq = 0;
    uint64_t master_tx_ts = 0;
    const uint16_t wrong_pan = (uint16_t)(UWB_PAN_ID ^ 0xFFFFu);

    build_sync_frame(buf, wrong_pan, UWB_ADDR_BROADCAST, UWB_FRAME_TYPE_SYNC,
                      0x0001u, 0x01ULL);

    int ret = uwb_sync_parse(buf, sizeof(buf), &cycle_seq, &master_tx_ts);

    zassert_equal(ret, -EBADMSG,
        "Expected -EBADMSG for wrong PAN ID; got %d", ret);
}

ZTEST(dw1000_sync, test_parse_wrong_dest_returns_ebadmsg)
{
    uint8_t buf[UWB_SYNC_FRAME_SIZE];
    uint16_t cycle_seq = 0;
    uint64_t master_tx_ts = 0;
    const uint16_t unicast_dest = 0x0042u;

    build_sync_frame(buf, UWB_PAN_ID, unicast_dest, UWB_FRAME_TYPE_SYNC,
                      0x0001u, 0x01ULL);

    int ret = uwb_sync_parse(buf, sizeof(buf), &cycle_seq, &master_tx_ts);

    zassert_equal(ret, -EBADMSG,
        "Expected -EBADMSG for non-broadcast destination; got %d", ret);
}

ZTEST(dw1000_sync, test_parse_rejects_null_args)
{
    uint8_t buf[UWB_SYNC_FRAME_SIZE];
    uint16_t cycle_seq = 0;
    uint64_t master_tx_ts = 0;

    build_sync_frame(buf, UWB_PAN_ID, UWB_ADDR_BROADCAST, UWB_FRAME_TYPE_SYNC,
                      0x0001u, 0x01ULL);

    zassert_equal(uwb_sync_parse(NULL, sizeof(buf), &cycle_seq, &master_tx_ts),
        -EINVAL, "NULL buf");
    zassert_equal(uwb_sync_parse(buf, sizeof(buf), NULL, &master_tx_ts),
        -EINVAL, "NULL cycle_seq");
    zassert_equal(uwb_sync_parse(buf, sizeof(buf), &cycle_seq, NULL),
        -EINVAL, "NULL master_tx_ts");
}

/* =============================================================================
 * dw1000_sync_rx() -- composition over the mocked dw1000_rx()
 * ========================================================================= */

ZTEST(dw1000_sync, test_sync_rx_valid_frame_returns_zero_and_populates_out)
{
    uint8_t frame[UWB_SYNC_FRAME_SIZE];
    uwb_sync_info_t info = {0};
    const uint16_t golden_cycle_seq = 0x0203u;
    const uint64_t golden_master_tx_ts = 0x0A0B0C0D0EULL;

    build_sync_frame(frame, UWB_PAN_ID, UWB_ADDR_BROADCAST, UWB_FRAME_TYPE_SYNC,
                      golden_cycle_seq, golden_master_tx_ts);

    mock_reg_state.sys_status = SYS_STATUS_RXFCG;
    mock_reg_state.rx_finfo   = (uint32)UWB_SYNC_FRAME_SIZE;
    memcpy(mock_readrxdata_state.injected_payload, frame, sizeof(frame));

    /* rx_ts bytes little-endian: byte[0] = LSB ... byte[4] = MSB. */
    mock_timestamp_state.rx_ts_bytes[0] = 0x11u;
    mock_timestamp_state.rx_ts_bytes[1] = 0x22u;
    mock_timestamp_state.rx_ts_bytes[2] = 0x33u;
    mock_timestamp_state.rx_ts_bytes[3] = 0x44u;
    mock_timestamp_state.rx_ts_bytes[4] = 0x05u;
    const uint64_t expected_rx_ts = 0x0544332211ULL;

    int ret = dw1000_sync_rx(&info, 1000);

    zassert_equal(ret, 0, "dw1000_sync_rx() returned %d (expected 0)", ret);
    zassert_equal(info.rx_ts, expected_rx_ts,
        "rx_ts: expected 0x%010llX; got 0x%010llX",
        (unsigned long long)expected_rx_ts, (unsigned long long)info.rx_ts);
    zassert_equal(info.cycle_seq, golden_cycle_seq,
        "cycle_seq: expected 0x%04X; got 0x%04X",
        golden_cycle_seq, info.cycle_seq);
    zassert_equal(info.master_tx_ts, golden_master_tx_ts,
        "master_tx_ts: expected 0x%010llX; got 0x%010llX",
        (unsigned long long)golden_master_tx_ts,
        (unsigned long long)info.master_tx_ts);
}

ZTEST(dw1000_sync, test_sync_rx_wrong_frame_type_returns_ebadmsg)
{
    uint8_t frame[UWB_SYNC_FRAME_SIZE];
    uwb_sync_info_t info = {0};

    build_sync_frame(frame, UWB_PAN_ID, UWB_ADDR_BROADCAST,
                      UWB_FRAME_TYPE_TAG_BLINK, 0x0001u, 0x01ULL);

    mock_reg_state.sys_status = SYS_STATUS_RXFCG;
    mock_reg_state.rx_finfo   = (uint32)UWB_SYNC_FRAME_SIZE;
    memcpy(mock_readrxdata_state.injected_payload, frame, sizeof(frame));

    int ret = dw1000_sync_rx(&info, 1000);

    zassert_equal(ret, -EBADMSG,
        "Expected -EBADMSG for wrong frame type; got %d", ret);
}

ZTEST(dw1000_sync, test_sync_rx_wrong_pan_returns_ebadmsg)
{
    uint8_t frame[UWB_SYNC_FRAME_SIZE];
    uwb_sync_info_t info = {0};
    const uint16_t wrong_pan = (uint16_t)(UWB_PAN_ID ^ 0xFFFFu);

    build_sync_frame(frame, wrong_pan, UWB_ADDR_BROADCAST, UWB_FRAME_TYPE_SYNC,
                      0x0001u, 0x01ULL);

    mock_reg_state.sys_status = SYS_STATUS_RXFCG;
    mock_reg_state.rx_finfo   = (uint32)UWB_SYNC_FRAME_SIZE;
    memcpy(mock_readrxdata_state.injected_payload, frame, sizeof(frame));

    int ret = dw1000_sync_rx(&info, 1000);

    zassert_equal(ret, -EBADMSG,
        "Expected -EBADMSG for wrong PAN ID; got %d", ret);
}

ZTEST(dw1000_sync, test_sync_rx_wrong_dest_returns_ebadmsg)
{
    uint8_t frame[UWB_SYNC_FRAME_SIZE];
    uwb_sync_info_t info = {0};
    const uint16_t unicast_dest = 0x0042u;

    build_sync_frame(frame, UWB_PAN_ID, unicast_dest, UWB_FRAME_TYPE_SYNC,
                      0x0001u, 0x01ULL);

    mock_reg_state.sys_status = SYS_STATUS_RXFCG;
    mock_reg_state.rx_finfo   = (uint32)UWB_SYNC_FRAME_SIZE;
    memcpy(mock_readrxdata_state.injected_payload, frame, sizeof(frame));

    int ret = dw1000_sync_rx(&info, 1000);

    zassert_equal(ret, -EBADMSG,
        "Expected -EBADMSG for non-broadcast destination; got %d", ret);
}

ZTEST(dw1000_sync, test_sync_rx_wrong_length_returns_ebadmsg)
{
    uint8_t frame[UWB_SYNC_FRAME_SIZE];
    uwb_sync_info_t info = {0};

    build_sync_frame(frame, UWB_PAN_ID, UWB_ADDR_BROADCAST, UWB_FRAME_TYPE_SYNC,
                      0x0001u, 0x01ULL);

    mock_reg_state.sys_status = SYS_STATUS_RXFCG;
    /* Report one byte short of a valid SYNC frame -- dw1000_rx() itself
     * succeeds (it has no opinion on application-layer framing), and
     * uwb_sync_parse()'s length check must reject it. */
    mock_reg_state.rx_finfo = (uint32)(UWB_SYNC_FRAME_SIZE - 1u);
    memcpy(mock_readrxdata_state.injected_payload, frame,
           UWB_SYNC_FRAME_SIZE - 1u);

    int ret = dw1000_sync_rx(&info, 1000);

    zassert_equal(ret, -EBADMSG,
        "Expected -EBADMSG for wrong length; got %d", ret);
}

ZTEST(dw1000_sync, test_sync_rx_mock_timeout_returns_etimedout)
{
    uwb_sync_info_t info = {0};

    /* SYS_STATUS_RXRFTO (Receive Frame Wait Timeout) is part of
     * SYS_STATUS_ALL_RX_TO -- see tests/dw1000_ranging for the same pattern. */
    mock_reg_state.sys_status = SYS_STATUS_RXRFTO;

    int ret = dw1000_sync_rx(&info, 1000);

    zassert_equal(ret, -ETIMEDOUT,
        "Expected -ETIMEDOUT (passthrough) on RX timeout; got %d", ret);
}

ZTEST(dw1000_sync, test_sync_rx_mock_rx_error_returns_eio)
{
    uwb_sync_info_t info = {0};

    /* SYS_STATUS_RXFCE (Receiver FCS Error) is part of SYS_STATUS_ALL_RX_ERR. */
    mock_reg_state.sys_status = SYS_STATUS_RXFCE;

    int ret = dw1000_sync_rx(&info, 1000);

    zassert_equal(ret, -EIO,
        "Expected -EIO (passthrough) on RX/FCS error; got %d", ret);
}

ZTEST(dw1000_sync, test_sync_rx_rejects_null_out)
{
    zassert_equal(dw1000_sync_rx(NULL, 1000), -EINVAL,
        "Expected -EINVAL for NULL out");
    zassert_equal(mock_rxenable_state.called, 0,
        "dwt_rxenable() must not be called on an invalid argument");
}
