/*! ----------------------------------------------------------------------------
 * @file    test_join_codec.c
 * @brief   ztest suite: Aloha join frame codec (UWB-259)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI).
 *
 * Covers:
 *   1.  uwb_build_join_request() — golden-byte comparison at the contract
 *       offsets (frame_type @ 9, src = 0xFFFE @ 7-8, dest = 0xFFFF @ 5-6,
 *       EUI-64 LE @ 10-17, capabilities @ 18), plus the returned length.
 *   2.  uwb_build_join_request() — reserved capability bits are forced to 0
 *       on TX even if the caller passes them set.
 *   3.  uwb_build_join_request() — NULL buf / NULL eui64 rejected.
 *   4.  uwb_parse_slot_assignment() — hand-built 24-byte SLOT_ASSIGNMENT
 *       recovers target_eui64, short_addr (LE-exercising value 0x0201),
 *       slot_idx, slot_count, slot_duration_us (LE, 500).
 *   5–6. uwb_parse_slot_assignment() — rejection (distinct negative return)
 *       of wrong length / wrong frame_type, and NULL buf / NULL out.
 *   7.  uwb_join_assignment_is_for_me() — true only on exact EUI-64 match;
 *       false on mismatch and on NULL inputs.
 *
 * No mocks are needed: uwb_join_codec.c does not touch SPI, GPIO, or
 * deca_driver — it is pure byte manipulation over the uwb_frames.h structs.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "uwb_frames.h"
#include "uwb_join_codec.h"

ZTEST_SUITE(join_codec, NULL, NULL, NULL, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Test 1 — build_join_request golden bytes / byte-exact contract offsets
 * --------------------------------------------------------------------------- */

ZTEST(join_codec, test_build_join_request_golden_bytes)
{
    uint8_t buf[UWB_JOIN_REQUEST_FRAME_SIZE];

    static const uint8_t eui64[UWB_EUI64_LEN] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
    };
    const uint8_t seq = 0x07;
    const uint8_t capabilities = UWB_JOIN_CAP_ROLE_REFERENCE;

    int ret = uwb_build_join_request(buf, eui64, seq, capabilities);

    zassert_equal(ret, (int)UWB_JOIN_REQUEST_FRAME_SIZE,
        "expected %d, got %d", (int)UWB_JOIN_REQUEST_FRAME_SIZE, ret);
    zassert_equal(ret, 19, "UWB_JOIN_REQUEST_FRAME_SIZE must be 19 bytes");

    /* Golden buffer, hand-built per contracts/uwb v0.6 §"Join request frame"
     * byte layout. */
    static const uint8_t golden[UWB_JOIN_REQUEST_FRAME_SIZE] = {
        0x41, 0x88,             /* Frame Control (LE 0x8841) */
        0x07,                   /* Sequence Number */
        0xEA, 0x5E,             /* PAN ID (LE 0x5EEA) */
        0xFF, 0xFF,             /* Destination Addr = broadcast (LE 0xFFFF) */
        0xFE, 0xFF,             /* Source Addr = unassigned (LE 0xFFFE) */
        0x20,                   /* Frame Type = UWB_FRAME_TYPE_JOIN_REQUEST */
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, /* EUI-64 LE @ offset 10 */
        0x01,                   /* capabilities = UWB_JOIN_CAP_ROLE_REFERENCE @ 18 */
    };

    zassert_mem_equal(buf, golden, sizeof(golden),
        "built JOIN_REQUEST frame does not match the golden buffer");

    /* Explicit contract-offset checks, per acceptance criteria. */
    zassert_equal(buf[UWB_OFF_FRAME_TYPE], 0x20, "frame_type @ offset 9");
    zassert_equal(buf[UWB_OFF_SRC_ADDR], 0xFE, "src_addr LSB @ offset 7");
    zassert_equal(buf[UWB_OFF_SRC_ADDR + 1], 0xFF, "src_addr MSB @ offset 8");
    zassert_equal(buf[UWB_OFF_DEST_ADDR], 0xFF, "dest_addr LSB @ offset 5");
    zassert_equal(buf[UWB_OFF_DEST_ADDR + 1], 0xFF, "dest_addr MSB @ offset 6");
    zassert_mem_equal(&buf[UWB_OFF_JOIN_EUI64], eui64, UWB_EUI64_LEN,
        "eui64 LE @ offset 10-17");
    zassert_equal(buf[UWB_OFF_JOIN_CAPABILITIES], capabilities,
        "capabilities @ offset 18");
}

/* ---------------------------------------------------------------------------
 * Test 2 — reserved capability bits forced to 0
 * --------------------------------------------------------------------------- */

ZTEST(join_codec, test_build_join_request_reserved_capability_bits_masked)
{
    uint8_t buf[UWB_JOIN_REQUEST_FRAME_SIZE];

    static const uint8_t eui64[UWB_EUI64_LEN] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
    };

    /* Set every bit, including reserved bits 7:1 — only bit 0 must survive. */
    const uint8_t capabilities_with_reserved = 0xFFU;

    int ret = uwb_build_join_request(buf, eui64, 0x00, capabilities_with_reserved);

    zassert_equal(ret, (int)UWB_JOIN_REQUEST_FRAME_SIZE,
        "expected %d, got %d", (int)UWB_JOIN_REQUEST_FRAME_SIZE, ret);

    zassert_equal(buf[UWB_OFF_JOIN_CAPABILITIES], UWB_JOIN_CAP_ROLE_REFERENCE,
        "reserved capability bits must transmit as 0; got 0x%02X",
        buf[UWB_OFF_JOIN_CAPABILITIES]);
}

/* ---------------------------------------------------------------------------
 * Test 3 — NULL buf / NULL eui64 on build
 * --------------------------------------------------------------------------- */

ZTEST(join_codec, test_build_join_request_null_buf)
{
    static const uint8_t eui64[UWB_EUI64_LEN] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
    };

    int ret = uwb_build_join_request(NULL, eui64, 0x00, 0x00);

    zassert_true(ret < 0, "expected negative return for NULL buf, got %d", ret);
}

ZTEST(join_codec, test_build_join_request_null_eui64)
{
    uint8_t buf[UWB_JOIN_REQUEST_FRAME_SIZE];

    int ret = uwb_build_join_request(buf, NULL, 0x00, 0x00);

    zassert_true(ret < 0, "expected negative return for NULL eui64, got %d", ret);
}

/* ---------------------------------------------------------------------------
 * Test 4 — parse_slot_assignment: hand-built frame, field recovery
 * --------------------------------------------------------------------------- */

/* Hand-built 24-byte SLOT_ASSIGNMENT per contracts/uwb v0.6
 * §"Slot / address assignment frame" byte layout. */
static const uint8_t slot_assignment_golden[UWB_SLOT_ASSIGNMENT_FRAME_SIZE] = {
    0x41, 0x88,             /* Frame Control (LE 0x8841) */
    0x09,                   /* Sequence Number */
    0xEA, 0x5E,             /* PAN ID (LE 0x5EEA) */
    0xFF, 0xFF,             /* Destination Addr = broadcast (LE 0xFFFF) */
    0x01, 0x00,             /* Source Addr = master short addr (LE 0x0001) */
    0x21,                   /* Frame Type = UWB_FRAME_TYPE_SLOT_ASSIGNMENT */
    0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11, /* target_eui64 @ offset 10 */
    0x01, 0x02,             /* short_addr (LE 0x0201) @ offset 18 */
    0x05,                   /* slot_idx @ offset 20 */
    0x18,                   /* slot_count = 24 @ offset 21 */
    0xF4, 0x01,             /* slot_duration_us (LE 500) @ offset 22 */
};

ZTEST(join_codec, test_parse_slot_assignment_field_recovery)
{
    uwb_slot_assignment_t parsed;

    int ret = uwb_parse_slot_assignment(slot_assignment_golden,
                                         sizeof(slot_assignment_golden),
                                         &parsed);

    zassert_equal(ret, 0, "expected 0, got %d", ret);

    static const uint8_t expected_eui64[UWB_EUI64_LEN] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11
    };
    zassert_mem_equal(parsed.target_eui64, expected_eui64, UWB_EUI64_LEN,
        "target_eui64 mismatch");
    zassert_equal(parsed.short_addr, 0x0201,
        "short_addr: expected 0x0201, got 0x%04X", parsed.short_addr);
    zassert_equal(parsed.slot_idx, 0x05, "slot_idx");
    zassert_equal(parsed.slot_count, 0x18, "slot_count");
    zassert_equal(parsed.slot_duration_us, 500,
        "slot_duration_us: expected 500, got %u", parsed.slot_duration_us);
}

/* ---------------------------------------------------------------------------
 * Tests 5–6 — uwb_parse_slot_assignment() rejection paths
 * --------------------------------------------------------------------------- */

ZTEST(join_codec, test_parse_slot_assignment_wrong_length)
{
    uwb_slot_assignment_t parsed;

    int ret = uwb_parse_slot_assignment(slot_assignment_golden,
                                         sizeof(slot_assignment_golden) - 1,
                                         &parsed);

    zassert_true(ret < 0, "expected negative return for wrong length, got %d", ret);
}

ZTEST(join_codec, test_parse_slot_assignment_wrong_frame_type)
{
    uint8_t buf[UWB_SLOT_ASSIGNMENT_FRAME_SIZE];
    memcpy(buf, slot_assignment_golden, sizeof(buf));
    buf[UWB_OFF_FRAME_TYPE] = (uint8_t)UWB_FRAME_TYPE_JOIN_REQUEST;

    uwb_slot_assignment_t parsed;
    int ret = uwb_parse_slot_assignment(buf, sizeof(buf), &parsed);

    zassert_true(ret < 0, "expected negative return for wrong frame_type, got %d", ret);
}

ZTEST(join_codec, test_parse_slot_assignment_wrong_length_and_frame_type_distinct)
{
    uwb_slot_assignment_t parsed;

    int ret_len = uwb_parse_slot_assignment(slot_assignment_golden,
                                             sizeof(slot_assignment_golden) - 1,
                                             &parsed);

    uint8_t buf[UWB_SLOT_ASSIGNMENT_FRAME_SIZE];
    memcpy(buf, slot_assignment_golden, sizeof(buf));
    buf[UWB_OFF_FRAME_TYPE] = (uint8_t)UWB_FRAME_TYPE_JOIN_REQUEST;
    int ret_type = uwb_parse_slot_assignment(buf, sizeof(buf), &parsed);

    zassert_true(ret_len < 0, "wrong-length must be rejected");
    zassert_true(ret_type < 0, "wrong-frame_type must be rejected");
    zassert_not_equal(ret_len, ret_type,
        "wrong length and wrong frame_type must return distinct error codes");
}

ZTEST(join_codec, test_parse_slot_assignment_null_buf)
{
    uwb_slot_assignment_t parsed;

    int ret = uwb_parse_slot_assignment(NULL, UWB_SLOT_ASSIGNMENT_FRAME_SIZE, &parsed);

    zassert_true(ret < 0, "expected negative return for NULL buf, got %d", ret);
}

ZTEST(join_codec, test_parse_slot_assignment_null_out)
{
    int ret = uwb_parse_slot_assignment(slot_assignment_golden,
                                         sizeof(slot_assignment_golden), NULL);

    zassert_true(ret < 0, "expected negative return for NULL out, got %d", ret);
}

/* ---------------------------------------------------------------------------
 * Test 7 — join_assignment_is_for_me() self-select rule
 * --------------------------------------------------------------------------- */

ZTEST(join_codec, test_join_assignment_is_for_me_match)
{
    uwb_slot_assignment_t parsed;
    int ret = uwb_parse_slot_assignment(slot_assignment_golden,
                                         sizeof(slot_assignment_golden),
                                         &parsed);
    zassert_equal(ret, 0, "expected 0, got %d", ret);

    static const uint8_t my_eui64[UWB_EUI64_LEN] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11
    };

    zassert_true(uwb_join_assignment_is_for_me(my_eui64, &parsed),
        "matching EUI-64 must be recognised as for me");
}

ZTEST(join_codec, test_join_assignment_is_for_me_mismatch)
{
    uwb_slot_assignment_t parsed;
    int ret = uwb_parse_slot_assignment(slot_assignment_golden,
                                         sizeof(slot_assignment_golden),
                                         &parsed);
    zassert_equal(ret, 0, "expected 0, got %d", ret);

    static const uint8_t other_eui64[UWB_EUI64_LEN] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    zassert_false(uwb_join_assignment_is_for_me(other_eui64, &parsed),
        "non-matching EUI-64 must not be recognised as for me");
}

ZTEST(join_codec, test_join_assignment_is_for_me_null_inputs)
{
    uwb_slot_assignment_t parsed;
    int ret = uwb_parse_slot_assignment(slot_assignment_golden,
                                         sizeof(slot_assignment_golden),
                                         &parsed);
    zassert_equal(ret, 0, "expected 0, got %d", ret);

    static const uint8_t my_eui64[UWB_EUI64_LEN] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11
    };

    zassert_false(uwb_join_assignment_is_for_me(NULL, &parsed),
        "NULL eui64_self must return false");
    zassert_false(uwb_join_assignment_is_for_me(my_eui64, NULL),
        "NULL parsed must return false");
}
