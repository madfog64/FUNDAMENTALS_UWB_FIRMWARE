/*! ----------------------------------------------------------------------------
 * @file    test_blink_codec.c
 * @brief   ztest suite: TDoA tag-blink frame codec (UWB-250)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI).
 *
 * Covers:
 *   1.  uwb_build_tag_blink() — round-trip via uwb_parse_tag_blink(): src
 *       address, blink_count (incl. a value exercising LE byte order,
 *       0x0201), flags (0x00 and UWB_BLINK_FLAG_LOW_BATTERY) all survive.
 *   2.  uwb_build_tag_blink() — golden-byte comparison at the contract
 *       offsets (frame_type @ 9, src @ 7-8, blink_count LE @ 10-11,
 *       flags @ 12), plus the returned length.
 *   3.  uwb_build_tag_blink() — reserved flag bits are forced to 0 on TX
 *       even if the caller passes them set.
 *   4.  uwb_build_tag_blink() — NULL buf rejected.
 *   5–6. uwb_parse_tag_blink() — rejection (distinct negative return) of
 *       wrong length / wrong frame_type, and NULL buf.
 *
 * No mocks are needed: uwb_blink_codec.c does not touch SPI, GPIO, or
 * deca_driver — it is pure byte manipulation over the uwb_frames.h structs.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <zephyr/ztest.h>

#include "uwb_frames.h"
#include "uwb_blink_codec.h"

ZTEST_SUITE(blink_codec, NULL, NULL, NULL, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Test 1 — round-trip
 * --------------------------------------------------------------------------- */

ZTEST(blink_codec, test_build_parse_round_trip)
{
    uint8_t buf[UWB_TAG_BLINK_FRAME_SIZE];

    const uint16_t src_addr = 0x1234;
    const uint8_t seq = 0x42;
    const uint16_t blink_count = 0x0201; /* exercises LE byte order */
    const uint8_t flags = UWB_BLINK_FLAG_LOW_BATTERY;

    int ret = uwb_build_tag_blink(buf, src_addr, seq, blink_count, flags);

    zassert_equal(ret, (int)UWB_TAG_BLINK_FRAME_SIZE,
        "expected %d, got %d", (int)UWB_TAG_BLINK_FRAME_SIZE, ret);

    uint16_t got_src_addr = 0;
    uint16_t got_blink_count = 0;
    uint8_t got_flags = 0xFF;

    int pret = uwb_parse_tag_blink(buf, sizeof(buf), &got_src_addr,
                                    &got_blink_count, &got_flags);

    zassert_equal(pret, 0, "expected 0, got %d", pret);
    zassert_equal(got_src_addr, src_addr, "src_addr");
    zassert_equal(got_blink_count, blink_count,
        "blink_count: expected 0x%04X, got 0x%04X", blink_count, got_blink_count);
    zassert_equal(got_flags, flags, "flags");
}

ZTEST(blink_codec, test_build_parse_round_trip_flags_zero)
{
    uint8_t buf[UWB_TAG_BLINK_FRAME_SIZE];

    const uint16_t src_addr = 0xBEEF;
    const uint8_t seq = 0x01;
    const uint16_t blink_count = 0x0000;
    const uint8_t flags = 0x00;

    int ret = uwb_build_tag_blink(buf, src_addr, seq, blink_count, flags);

    zassert_equal(ret, (int)UWB_TAG_BLINK_FRAME_SIZE,
        "expected %d, got %d", (int)UWB_TAG_BLINK_FRAME_SIZE, ret);

    uint16_t got_src_addr = 0;
    uint16_t got_blink_count = 0;
    uint8_t got_flags = 0xFF;

    int pret = uwb_parse_tag_blink(buf, sizeof(buf), &got_src_addr,
                                    &got_blink_count, &got_flags);

    zassert_equal(pret, 0, "expected 0, got %d", pret);
    zassert_equal(got_src_addr, src_addr, "src_addr");
    zassert_equal(got_blink_count, blink_count, "blink_count");
    zassert_equal(got_flags, flags, "flags");
}

/* ---------------------------------------------------------------------------
 * Test 2 — golden bytes / byte-exact contract offsets
 * --------------------------------------------------------------------------- */

ZTEST(blink_codec, test_build_tag_blink_golden_bytes)
{
    uint8_t buf[UWB_TAG_BLINK_FRAME_SIZE];

    const uint16_t src_addr = 0x5678;
    const uint8_t seq = 0x07;
    const uint16_t blink_count = 0x0201;
    const uint8_t flags = UWB_BLINK_FLAG_LOW_BATTERY;

    int ret = uwb_build_tag_blink(buf, src_addr, seq, blink_count, flags);

    zassert_equal(ret, (int)UWB_TAG_BLINK_FRAME_SIZE,
        "expected %d, got %d", (int)UWB_TAG_BLINK_FRAME_SIZE, ret);
    zassert_equal(ret, 13, "UWB_TAG_BLINK_FRAME_SIZE must be 13 bytes");

    /* Golden buffer, hand-built per contracts/uwb/README.md
     * §"TDoA tag blink frame" byte layout. */
    static const uint8_t golden[UWB_TAG_BLINK_FRAME_SIZE] = {
        0x41, 0x88,             /* Frame Control (LE 0x8841) */
        0x07,                   /* Sequence Number */
        0xEA, 0x5E,             /* PAN ID (LE 0x5EEA) */
        0xFF, 0xFF,             /* Destination Addr = broadcast (LE 0xFFFF) */
        0x78, 0x56,             /* Source Addr = tag (LE 0x5678) */
        0x02,                   /* Frame Type = UWB_FRAME_TYPE_TAG_BLINK */
        0x01, 0x02,             /* blink_count (LE 0x0201) @ offset 10 */
        0x01,                   /* flags = UWB_BLINK_FLAG_LOW_BATTERY @ offset 12 */
    };

    zassert_mem_equal(buf, golden, sizeof(golden),
        "built TAG_BLINK frame does not match the golden buffer");

    /* Explicit contract-offset checks, per acceptance criteria. */
    zassert_equal(buf[UWB_OFF_FRAME_TYPE], 0x02, "frame_type @ offset 9");
    zassert_equal(buf[UWB_OFF_SRC_ADDR], 0x78, "src_addr LSB @ offset 7");
    zassert_equal(buf[UWB_OFF_SRC_ADDR + 1], 0x56, "src_addr MSB @ offset 8");
    zassert_equal(buf[UWB_OFF_BLINK_COUNT], 0x01, "blink_count LSB @ offset 10");
    zassert_equal(buf[UWB_OFF_BLINK_COUNT + 1], 0x02, "blink_count MSB @ offset 11");
    zassert_equal(buf[UWB_OFF_BLINK_FLAGS], 0x01, "flags @ offset 12");
}

/* ---------------------------------------------------------------------------
 * Test 3 — reserved flag bits forced to 0
 * --------------------------------------------------------------------------- */

ZTEST(blink_codec, test_build_tag_blink_reserved_flags_masked)
{
    uint8_t buf[UWB_TAG_BLINK_FRAME_SIZE];

    /* Set every bit, including reserved bits 7:1 — only bit 0 must survive. */
    const uint8_t flags_with_reserved = 0xFFU;

    int ret = uwb_build_tag_blink(buf, 0x0001, 0x00, 0x0000, flags_with_reserved);

    zassert_equal(ret, (int)UWB_TAG_BLINK_FRAME_SIZE,
        "expected %d, got %d", (int)UWB_TAG_BLINK_FRAME_SIZE, ret);

    zassert_equal(buf[UWB_OFF_BLINK_FLAGS], UWB_BLINK_FLAG_LOW_BATTERY,
        "reserved flag bits must transmit as 0; got 0x%02X",
        buf[UWB_OFF_BLINK_FLAGS]);
}

/* ---------------------------------------------------------------------------
 * Test 4 — NULL buf on build
 * --------------------------------------------------------------------------- */

ZTEST(blink_codec, test_build_tag_blink_null_buf)
{
    int ret = uwb_build_tag_blink(NULL, 0x0001, 0x00, 0x0000, 0x00);

    zassert_true(ret < 0, "expected negative return for NULL buf, got %d", ret);
}

/* ---------------------------------------------------------------------------
 * Tests 5–6 — uwb_parse_tag_blink() rejection paths
 * --------------------------------------------------------------------------- */

ZTEST(blink_codec, test_parse_tag_blink_wrong_length)
{
    uint8_t buf[UWB_TAG_BLINK_FRAME_SIZE];

    uwb_build_tag_blink(buf, 0x1111, 0x01, 0x2222, 0x00);

    int ret = uwb_parse_tag_blink(buf, sizeof(buf) - 1, NULL, NULL, NULL);

    zassert_true(ret < 0, "expected negative return for wrong length, got %d", ret);
}

ZTEST(blink_codec, test_parse_tag_blink_wrong_frame_type)
{
    uint8_t buf[UWB_TAG_BLINK_FRAME_SIZE];

    uwb_build_tag_blink(buf, 0x1111, 0x01, 0x2222, 0x00);
    buf[UWB_OFF_FRAME_TYPE] = (uint8_t)UWB_FRAME_TYPE_SYNC;

    int ret = uwb_parse_tag_blink(buf, sizeof(buf), NULL, NULL, NULL);

    zassert_true(ret < 0, "expected negative return for wrong frame_type, got %d", ret);
}

ZTEST(blink_codec, test_parse_tag_blink_wrong_length_and_frame_type_distinct)
{
    uint8_t buf[UWB_TAG_BLINK_FRAME_SIZE];

    uwb_build_tag_blink(buf, 0x1111, 0x01, 0x2222, 0x00);

    int ret_len = uwb_parse_tag_blink(buf, sizeof(buf) - 1, NULL, NULL, NULL);

    buf[UWB_OFF_FRAME_TYPE] = (uint8_t)UWB_FRAME_TYPE_SYNC;
    int ret_type = uwb_parse_tag_blink(buf, sizeof(buf), NULL, NULL, NULL);

    zassert_true(ret_len < 0, "wrong-length must be rejected");
    zassert_true(ret_type < 0, "wrong-frame_type must be rejected");
    zassert_not_equal(ret_len, ret_type,
        "wrong length and wrong frame_type must return distinct error codes");
}

ZTEST(blink_codec, test_parse_tag_blink_null_buf)
{
    int ret = uwb_parse_tag_blink(NULL, UWB_TAG_BLINK_FRAME_SIZE, NULL, NULL, NULL);

    zassert_true(ret < 0, "expected negative return for NULL buf, got %d", ret);
}
