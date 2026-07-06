/*! ----------------------------------------------------------------------------
 * @file    test_twr_codec.c
 * @brief   ztest suite: DS-TWR frame codec (UWB-230)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI).
 *
 * Covers:
 *   1–3.  uint40 LE timestamp helpers — boundary values (0, 2^40-1) and
 *         masking of inputs above 2^40.
 *   4–5.  uwb_build_twr_response() — round-trip (fields read back match
 *         inputs) and a golden-byte comparison at the contract offsets
 *         (exchange_id @ 10, poll_rx_ts @ 12, resp_tx_ts @ 17), plus the
 *         returned length.
 *   6–9.  uwb_parse_twr_poll() — valid parse, and rejection (negative
 *         return) of wrong length / wrong frame_type / wrong PAN ID.
 *  10–13. uwb_parse_twr_final() — valid parse, and the same three
 *         rejection cases.
 *
 * No mocks are needed: uwb_twr_codec.c does not touch SPI, GPIO, or
 * deca_driver — it is pure byte manipulation over the uwb_frames.h structs.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <zephyr/ztest.h>

#include "uwb_frames.h"
#include "uwb_twr_codec.h"

ZTEST_SUITE(twr_codec, NULL, NULL, NULL, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Local helpers — only used to construct raw test buffers; not exported by
 * the module under test.
 * --------------------------------------------------------------------------- */

static void local_write_le16(uint8_t out[2], uint16_t v)
{
    out[0] = (uint8_t)(v & 0xFFU);
    out[1] = (uint8_t)((v >> 8) & 0xFFU);
}

/* ---------------------------------------------------------------------------
 * Tests 1–3 — uint40 little-endian helpers
 * --------------------------------------------------------------------------- */

ZTEST(twr_codec, test_ts40_round_trip_zero)
{
    uint8_t buf[5];

    uwb_ts_write40(buf, 0);

    for (int i = 0; i < 5; i++) {
        zassert_equal(buf[i], 0, "byte[%d] expected 0x00, got 0x%02X", i, buf[i]);
    }

    uint64_t ts = uwb_ts_read40(buf);

    zassert_equal(ts, 0ULL, "expected 0, got %llu", (unsigned long long)ts);
}

ZTEST(twr_codec, test_ts40_round_trip_max)
{
    const uint64_t max40 = 0xFFFFFFFFFFULL; /* 2^40 - 1 */
    uint8_t buf[5];

    uwb_ts_write40(buf, max40);

    for (int i = 0; i < 5; i++) {
        zassert_equal(buf[i], 0xFFU, "byte[%d] expected 0xFF, got 0x%02X", i, buf[i]);
    }

    uint64_t ts = uwb_ts_read40(buf);

    zassert_equal(ts, max40,
        "expected 0x%llX, got 0x%llX", (unsigned long long)max40, (unsigned long long)ts);
}

ZTEST(twr_codec, test_ts40_write_masks_overflow)
{
    /* Value with bits set above bit 39 — must be masked to 2^40-1 on write. */
    const uint64_t over = 0x1FF0102030405ULL;
    const uint64_t expected = over & 0xFFFFFFFFFFULL;
    uint8_t buf[5];

    uwb_ts_write40(buf, over);

    uint64_t ts = uwb_ts_read40(buf);

    zassert_equal(ts, expected,
        "expected masked 0x%llX, got 0x%llX",
        (unsigned long long)expected, (unsigned long long)ts);
}

ZTEST(twr_codec, test_ts40_round_trip_arbitrary)
{
    const uint64_t val = 0x0A1B2C3D4EULL;
    uint8_t buf[5];

    uwb_ts_write40(buf, val);
    uint64_t ts = uwb_ts_read40(buf);

    zassert_equal(ts, val,
        "expected 0x%llX, got 0x%llX", (unsigned long long)val, (unsigned long long)ts);
}

/* ---------------------------------------------------------------------------
 * Tests 4–5 — uwb_build_twr_response()
 * --------------------------------------------------------------------------- */

ZTEST(twr_codec, test_build_twr_response_round_trip)
{
    uint8_t buf[UWB_TWR_RESPONSE_FRAME_SIZE];

    const uint16_t exchange_id = 0x1234;
    const uint16_t initiator_addr = 0x0011;
    const uint16_t responder_addr = 0x0022;
    const uint8_t seq = 0x42;
    const uint64_t poll_rx_ts = 0x0102030405ULL;
    const uint64_t resp_tx_ts = 0x06070809AAULL;

    int ret = uwb_build_twr_response(buf, exchange_id, initiator_addr,
                                      responder_addr, seq,
                                      poll_rx_ts, resp_tx_ts);

    zassert_equal(ret, (int)UWB_TWR_RESPONSE_FRAME_SIZE,
        "expected %d, got %d", (int)UWB_TWR_RESPONSE_FRAME_SIZE, ret);

    const uwb_twr_resp_frame_t *frame = (const uwb_twr_resp_frame_t *)buf;

    zassert_equal(frame->hdr.mac.frame_ctrl[0], UWB_FRAME_CTRL_LOW, "FC low byte");
    zassert_equal(frame->hdr.mac.frame_ctrl[1], UWB_FRAME_CTRL_HIGH, "FC high byte");
    zassert_equal(frame->hdr.mac.seq_num, seq, "seq_num");

    uint16_t pan = (uint16_t)(frame->hdr.mac.pan_id[0] | (frame->hdr.mac.pan_id[1] << 8));
    zassert_equal(pan, UWB_PAN_ID, "PAN ID");

    uint16_t dest = (uint16_t)(frame->hdr.mac.dest_addr[0] | (frame->hdr.mac.dest_addr[1] << 8));
    zassert_equal(dest, initiator_addr, "dest_addr must be the initiator address");

    uint16_t src = (uint16_t)(frame->hdr.mac.src_addr[0] | (frame->hdr.mac.src_addr[1] << 8));
    zassert_equal(src, responder_addr, "src_addr must be the responder address");

    zassert_equal(frame->hdr.frame_type, (uint8_t)UWB_FRAME_TYPE_TWR_RESPONSE, "frame_type");

    uint16_t got_exchange_id = (uint16_t)(frame->payload.exchange_id[0] |
                                          (frame->payload.exchange_id[1] << 8));
    zassert_equal(got_exchange_id, exchange_id, "exchange_id");

    uint64_t got_poll_rx_ts = uwb_ts_read40(frame->payload.poll_rx_ts);
    zassert_equal(got_poll_rx_ts, poll_rx_ts,
        "poll_rx_ts: expected 0x%llX, got 0x%llX",
        (unsigned long long)poll_rx_ts, (unsigned long long)got_poll_rx_ts);

    uint64_t got_resp_tx_ts = uwb_ts_read40(frame->payload.resp_tx_ts);
    zassert_equal(got_resp_tx_ts, resp_tx_ts,
        "resp_tx_ts: expected 0x%llX, got 0x%llX",
        (unsigned long long)resp_tx_ts, (unsigned long long)got_resp_tx_ts);
}

ZTEST(twr_codec, test_build_twr_response_golden_bytes)
{
    uint8_t buf[UWB_TWR_RESPONSE_FRAME_SIZE];

    const uint16_t exchange_id = 0xABCD;
    const uint16_t initiator_addr = 0x1234;
    const uint16_t responder_addr = 0x5678;
    const uint8_t seq = 0x07;
    const uint64_t poll_rx_ts = 0x0102030405ULL;
    const uint64_t resp_tx_ts = 0x0A0B0C0D0EULL;

    int ret = uwb_build_twr_response(buf, exchange_id, initiator_addr,
                                      responder_addr, seq,
                                      poll_rx_ts, resp_tx_ts);

    zassert_equal(ret, (int)UWB_TWR_RESPONSE_FRAME_SIZE,
        "expected %d, got %d", (int)UWB_TWR_RESPONSE_FRAME_SIZE, ret);
    zassert_equal(ret, 22, "UWB_TWR_RESPONSE_FRAME_SIZE must be 22 bytes");

    /* Golden buffer, hand-built per contracts/uwb/README.md
     * §"DS-TWR calibration frames" byte layout. */
    static const uint8_t golden[UWB_TWR_RESPONSE_FRAME_SIZE] = {
        0x41, 0x88,             /* Frame Control (LE 0x8841) */
        0x07,                   /* Sequence Number */
        0xEA, 0x5E,             /* PAN ID (LE 0x5EEA) */
        0x34, 0x12,             /* Destination Addr = initiator (LE 0x1234) */
        0x78, 0x56,             /* Source Addr = responder (LE 0x5678) */
        0x11,                   /* Frame Type = UWB_FRAME_TYPE_TWR_RESPONSE */
        0xCD, 0xAB,             /* Exchange ID (LE 0xABCD) @ offset 10 */
        0x05, 0x04, 0x03, 0x02, 0x01, /* poll_rx_ts T2 (LE 0x0102030405) @ offset 12 */
        0x0E, 0x0D, 0x0C, 0x0B, 0x0A, /* resp_tx_ts T3 (LE 0x0A0B0C0D0E) @ offset 17 */
    };

    zassert_mem_equal(buf, golden, sizeof(golden),
        "built RESPONSE frame does not match the golden buffer");

    /* Explicit contract-offset checks, per acceptance criteria. */
    zassert_equal(buf[UWB_OFF_TWR_EXCHANGE_ID], 0xCD, "exchange_id LSB @ offset 10");
    zassert_equal(buf[UWB_OFF_TWR_EXCHANGE_ID + 1], 0xAB, "exchange_id MSB @ offset 11");
    zassert_equal(buf[UWB_OFF_TWR_RESP_POLL_RX_TS], 0x05, "poll_rx_ts LSB @ offset 12");
    zassert_equal(buf[UWB_OFF_TWR_RESP_RESP_TX_TS], 0x0E, "resp_tx_ts LSB @ offset 17");
}

ZTEST(twr_codec, test_build_twr_response_null_buf)
{
    int ret = uwb_build_twr_response(NULL, 1, 2, 3, 4, 5, 6);

    zassert_true(ret < 0, "expected negative return for NULL buf, got %d", ret);
}

/* ---------------------------------------------------------------------------
 * Test buffer builders — POLL and FINAL frames (no build_* API exists for
 * these frame types in scope; construct raw bytes here to exercise parse).
 * --------------------------------------------------------------------------- */

static void build_poll_frame(uint8_t out[UWB_TWR_POLL_FRAME_SIZE],
                              uint16_t initiator_addr, uint16_t responder_addr,
                              uint8_t seq, uint16_t exchange_id)
{
    uwb_twr_poll_frame_t *frame = (uwb_twr_poll_frame_t *)out;

    frame->hdr.mac.frame_ctrl[0] = UWB_FRAME_CTRL_LOW;
    frame->hdr.mac.frame_ctrl[1] = UWB_FRAME_CTRL_HIGH;
    frame->hdr.mac.seq_num = seq;
    local_write_le16(frame->hdr.mac.pan_id, UWB_PAN_ID);
    local_write_le16(frame->hdr.mac.dest_addr, responder_addr);
    local_write_le16(frame->hdr.mac.src_addr, initiator_addr);
    frame->hdr.frame_type = (uint8_t)UWB_FRAME_TYPE_TWR_POLL;
    local_write_le16(frame->payload.exchange_id, exchange_id);
}

static void build_final_frame(uint8_t out[UWB_TWR_FINAL_FRAME_SIZE],
                               uint16_t initiator_addr, uint16_t responder_addr,
                               uint8_t seq, uint16_t exchange_id,
                               uint64_t poll_tx_ts, uint64_t resp_rx_ts,
                               uint64_t final_tx_ts)
{
    uwb_twr_final_frame_t *frame = (uwb_twr_final_frame_t *)out;

    frame->hdr.mac.frame_ctrl[0] = UWB_FRAME_CTRL_LOW;
    frame->hdr.mac.frame_ctrl[1] = UWB_FRAME_CTRL_HIGH;
    frame->hdr.mac.seq_num = seq;
    local_write_le16(frame->hdr.mac.pan_id, UWB_PAN_ID);
    local_write_le16(frame->hdr.mac.dest_addr, responder_addr);
    local_write_le16(frame->hdr.mac.src_addr, initiator_addr);
    frame->hdr.frame_type = (uint8_t)UWB_FRAME_TYPE_TWR_FINAL;
    local_write_le16(frame->payload.exchange_id, exchange_id);
    uwb_ts_write40(frame->payload.poll_tx_ts, poll_tx_ts);
    uwb_ts_write40(frame->payload.resp_rx_ts, resp_rx_ts);
    uwb_ts_write40(frame->payload.final_tx_ts, final_tx_ts);
}

/* ---------------------------------------------------------------------------
 * Tests 6–9 — uwb_parse_twr_poll()
 * --------------------------------------------------------------------------- */

ZTEST(twr_codec, test_parse_twr_poll_valid)
{
    uint8_t buf[UWB_TWR_POLL_FRAME_SIZE];
    const uint16_t initiator_addr = 0xAAAA;
    const uint16_t responder_addr = 0xBBBB;
    const uint16_t exchange_id = 0x5555;

    build_poll_frame(buf, initiator_addr, responder_addr, 0x01, exchange_id);

    uint16_t got_initiator = 0;
    uint16_t got_exchange_id = 0;

    int ret = uwb_parse_twr_poll(buf, sizeof(buf), &got_initiator, &got_exchange_id);

    zassert_equal(ret, 0, "expected 0, got %d", ret);
    zassert_equal(got_initiator, initiator_addr, "initiator_addr");
    zassert_equal(got_exchange_id, exchange_id, "exchange_id");
}

ZTEST(twr_codec, test_parse_twr_poll_wrong_length)
{
    uint8_t buf[UWB_TWR_POLL_FRAME_SIZE];

    build_poll_frame(buf, 0x1111, 0x2222, 0x01, 0x3333);

    int ret = uwb_parse_twr_poll(buf, sizeof(buf) - 1, NULL, NULL);

    zassert_true(ret < 0, "expected negative return for wrong length, got %d", ret);
}

ZTEST(twr_codec, test_parse_twr_poll_wrong_frame_type)
{
    uint8_t buf[UWB_TWR_POLL_FRAME_SIZE];

    build_poll_frame(buf, 0x1111, 0x2222, 0x01, 0x3333);
    buf[UWB_OFF_FRAME_TYPE] = (uint8_t)UWB_FRAME_TYPE_TWR_RESPONSE;

    int ret = uwb_parse_twr_poll(buf, sizeof(buf), NULL, NULL);

    zassert_true(ret < 0, "expected negative return for wrong frame_type, got %d", ret);
}

ZTEST(twr_codec, test_parse_twr_poll_wrong_pan)
{
    uint8_t buf[UWB_TWR_POLL_FRAME_SIZE];

    build_poll_frame(buf, 0x1111, 0x2222, 0x01, 0x3333);
    buf[UWB_OFF_PAN_ID] = 0x00;
    buf[UWB_OFF_PAN_ID + 1] = 0x00;

    int ret = uwb_parse_twr_poll(buf, sizeof(buf), NULL, NULL);

    zassert_true(ret < 0, "expected negative return for wrong PAN ID, got %d", ret);
}

ZTEST(twr_codec, test_parse_twr_poll_null_buf)
{
    int ret = uwb_parse_twr_poll(NULL, UWB_TWR_POLL_FRAME_SIZE, NULL, NULL);

    zassert_true(ret < 0, "expected negative return for NULL buf, got %d", ret);
}

/* ---------------------------------------------------------------------------
 * Tests 10–13 — uwb_parse_twr_final()
 * --------------------------------------------------------------------------- */

ZTEST(twr_codec, test_parse_twr_final_valid)
{
    uint8_t buf[UWB_TWR_FINAL_FRAME_SIZE];
    const uint16_t exchange_id = 0x9999;
    const uint64_t poll_tx_ts = 0x0102030405ULL;
    const uint64_t resp_rx_ts = 0x1112131415ULL;
    const uint64_t final_tx_ts = 0x2122232425ULL;

    build_final_frame(buf, 0x1111, 0x2222, 0x02, exchange_id,
                       poll_tx_ts, resp_rx_ts, final_tx_ts);

    uint16_t got_exchange_id = 0;
    uint64_t got_poll_tx_ts = 0;
    uint64_t got_resp_rx_ts = 0;
    uint64_t got_final_tx_ts = 0;

    int ret = uwb_parse_twr_final(buf, sizeof(buf), &got_exchange_id,
                                   &got_poll_tx_ts, &got_resp_rx_ts, &got_final_tx_ts);

    zassert_equal(ret, 0, "expected 0, got %d", ret);
    zassert_equal(got_exchange_id, exchange_id, "exchange_id");
    zassert_equal(got_poll_tx_ts, poll_tx_ts,
        "poll_tx_ts: expected 0x%llX, got 0x%llX",
        (unsigned long long)poll_tx_ts, (unsigned long long)got_poll_tx_ts);
    zassert_equal(got_resp_rx_ts, resp_rx_ts,
        "resp_rx_ts: expected 0x%llX, got 0x%llX",
        (unsigned long long)resp_rx_ts, (unsigned long long)got_resp_rx_ts);
    zassert_equal(got_final_tx_ts, final_tx_ts,
        "final_tx_ts: expected 0x%llX, got 0x%llX",
        (unsigned long long)final_tx_ts, (unsigned long long)got_final_tx_ts);
}

ZTEST(twr_codec, test_parse_twr_final_wrong_length)
{
    uint8_t buf[UWB_TWR_FINAL_FRAME_SIZE];

    build_final_frame(buf, 0x1111, 0x2222, 0x02, 0x3333,
                       0x01ULL, 0x02ULL, 0x03ULL);

    int ret = uwb_parse_twr_final(buf, sizeof(buf) + 1, NULL, NULL, NULL, NULL);

    zassert_true(ret < 0, "expected negative return for wrong length, got %d", ret);
}

ZTEST(twr_codec, test_parse_twr_final_wrong_frame_type)
{
    uint8_t buf[UWB_TWR_FINAL_FRAME_SIZE];

    build_final_frame(buf, 0x1111, 0x2222, 0x02, 0x3333,
                       0x01ULL, 0x02ULL, 0x03ULL);
    buf[UWB_OFF_FRAME_TYPE] = (uint8_t)UWB_FRAME_TYPE_TWR_POLL;

    int ret = uwb_parse_twr_final(buf, sizeof(buf), NULL, NULL, NULL, NULL);

    zassert_true(ret < 0, "expected negative return for wrong frame_type, got %d", ret);
}

ZTEST(twr_codec, test_parse_twr_final_wrong_pan)
{
    uint8_t buf[UWB_TWR_FINAL_FRAME_SIZE];

    build_final_frame(buf, 0x1111, 0x2222, 0x02, 0x3333,
                       0x01ULL, 0x02ULL, 0x03ULL);
    buf[UWB_OFF_PAN_ID] = 0xFF;
    buf[UWB_OFF_PAN_ID + 1] = 0xFF;

    int ret = uwb_parse_twr_final(buf, sizeof(buf), NULL, NULL, NULL, NULL);

    zassert_true(ret < 0, "expected negative return for wrong PAN ID, got %d", ret);
}

ZTEST(twr_codec, test_parse_twr_final_null_buf)
{
    int ret = uwb_parse_twr_final(NULL, UWB_TWR_FINAL_FRAME_SIZE,
                                   NULL, NULL, NULL, NULL);

    zassert_true(ret < 0, "expected negative return for NULL buf, got %d", ret);
}
