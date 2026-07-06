/*! ----------------------------------------------------------------------------
 * @file    test_twr_responder.c
 * @brief   ztest suite: DS-TWR responder mode state machine (UWB-232)
 *
 * Platform: unit_testing (native-hosted, no Zephyr kernel, no real SPI).
 * Compiles the real twr_responder.c + dw1000_ranging.c (UWB-231) +
 * uwb_twr_codec.c (UWB-230) against the extended deca_driver mock (see
 * mock_deca_driver.h) — an RX *queue*, since a single
 * twr_responder_run_once() attempt can call dw1000_rx() up to twice (POLL
 * wait, FINAL wait).
 *
 * Covers:
 *   - Full exchange happy path: POLL -> RESPONSE scheduled with the right
 *     exchange_id / T2 / T3=T2+delay, matching FINAL -> T1/T4/T5 extracted +
 *     T6 captured, debug-range formula matches a hand-calc fixture
 *     (CONFIG_UWB_TWR_RESPONDER_DEBUG_RANGE=1, injected in CMakeLists.txt).
 *   - No POLL heard (timeout) -> TWR_RESPONDER_NO_POLL, no RESPONSE built.
 *   - POLL heard but wrong destination address -> TWR_RESPONDER_FOREIGN_FRAME.
 *   - Malformed/wrong-frame-type frame while waiting for POLL ->
 *     TWR_RESPONDER_FOREIGN_FRAME.
 *   - RESPONSE scheduling fails (HPDWARN) -> TWR_RESPONDER_TX_ERROR, no FINAL
 *     wait attempted.
 *   - POLL answered but no FINAL before timeout -> TWR_RESPONDER_NO_FINAL.
 *   - FINAL heard but exchange_id mismatch -> TWR_RESPONDER_FINAL_MISMATCH.
 *   - out == NULL does not crash the happy path.
 *   - uwb_twr_range_mm() as a pure function: hand-calc fixture + degenerate
 *     (all-zero deltas) denominator guard.
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
#include "twr_responder.h"
#include "uwb_frames.h"
#include "uwb_twr_codec.h"

/* ---------------------------------------------------------------------------
 * Suite setup / teardown
 * --------------------------------------------------------------------------- */
static void before_each(void *fixture)
{
    (void)fixture;
    mock_deca_reset();
}

ZTEST_SUITE(twr_responder, NULL, NULL, before_each, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Test frame builders -- hand-packed byte buffers, deliberately independent
 * of twr_responder.c's own use of uwb_build_twr_response()/uwb_parse_twr_*()
 * so these tests do not become tautological. Mirrors
 * tests/twr_codec/src/test_twr_codec.c's build_poll_frame()/build_final_frame().
 * --------------------------------------------------------------------------- */

static void local_write_le16(uint8_t out[2], uint16_t v)
{
    out[0] = (uint8_t)(v & 0xFFU);
    out[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static uint16_t local_read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void ts_to_le5(uint8_t out[5], uint64_t ts)
{
    uwb_ts_write40(out, ts);
}

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

/* Convenience: push a well-formed good-frame RX event. */
static void push_good_frame(const uint8_t *buf, uint16_t len, uint64_t rx_ts)
{
    uint8_t ts_bytes[5];

    ts_to_le5(ts_bytes, rx_ts);
    mock_rx_queue_push(SYS_STATUS_RXFCG, (uint32)len, buf, len, ts_bytes);
}

/* ---------------------------------------------------------------------------
 * Test 1 -- full exchange happy path + debug-range formula
 * --------------------------------------------------------------------------- */
ZTEST(twr_responder, test_full_exchange_builds_response_and_captures_final)
{
    const uint16_t self_addr = 0x0002;
    const uint16_t initiator_addr = 0x0001;
    const uint16_t exchange_id = 0x1234;

    /* T2: this responder's POLL RX timestamp -- arbitrary but realistic
     * magnitude (well below the 40-bit range, no wraparound). */
    const uint64_t t2 = 1000000ULL;

    /* T3 is *derived*, not chosen: twr_responder.c computes it as
     * dw1000_delayed_tx_time(T2, CONFIG_UWB_TWR_RESP_REPLY_DELAY_DTU) --
     * calling the same (already unit-tested in tests/dw1000_ranging, UWB-231)
     * pure function here keeps this fixture correct even if the Kconfig
     * default reply delay changes. */
    const uint64_t t3 = dw1000_delayed_tx_time(t2, CONFIG_UWB_TWR_RESP_REPLY_DELAY_DTU);

    /* Construct T1/T4/T5/T6 for a clean hand-calc ToF: mirror the
     * initiator's reply delay (T5-T4) against the responder's own (T3-T2),
     * and use the same round-trip (T4-T1 == T6-T3) on both sides. Under that
     * symmetry the DS-TWR formula collapses to ToF = (T_round - T_reply)/2
     * (see uwb_frames.h's DS-TWR range formula comment for the general
     * form); picking T_round = T_reply + 2000 ticks gives an exact,
     * remainder-free ToF = 1000 ticks. */
    const uint64_t t_reply = t3 - t2;
    const uint64_t desired_tof_ticks = 1000ULL;
    const uint64_t t_round = t_reply + (2ULL * desired_tof_ticks);

    const uint64_t t1 = 500000ULL;             /* initiator's POLL TX timestamp */
    const uint64_t t4 = t1 + t_round;          /* initiator's RESPONSE RX timestamp */
    const uint64_t t5 = t4 + t_reply;          /* initiator's scheduled FINAL TX timestamp */
    const uint64_t t6 = t3 + t_round;          /* this responder's FINAL RX timestamp */

    uint8_t poll_buf[UWB_TWR_POLL_FRAME_SIZE];
    uint8_t final_buf[UWB_TWR_FINAL_FRAME_SIZE];

    build_poll_frame(poll_buf, initiator_addr, self_addr, 0x01, exchange_id);
    push_good_frame(poll_buf, sizeof(poll_buf), t2);

    build_final_frame(final_buf, initiator_addr, self_addr, 0x02, exchange_id,
                       t1, t4, t5);
    push_good_frame(final_buf, sizeof(final_buf), t6);

    uwb_twr_exchange_t out;
    twr_responder_status_t status = twr_responder_run_once(self_addr, &out);

    zassert_equal(status, TWR_RESPONDER_EXCHANGE_OK,
        "expected TWR_RESPONDER_EXCHANGE_OK, got %d", (int)status);

    zassert_equal(mock_rxenable_state.called, 2,
        "expected two dw1000_rx() attempts (POLL wait + FINAL wait); got %d",
        mock_rxenable_state.called);

    /* --- RESPONSE scheduling: right exchange_id, T2, T3=T2+delay --------- */
    zassert_equal(mock_delayedtrxtime_state.called, 1,
        "expected dwt_setdelayedtrxtime() called once; got %d",
        mock_delayedtrxtime_state.called);
    zassert_equal(mock_delayedtrxtime_state.starttime, (uint32)(t3 >> 8),
        "dwt_setdelayedtrxtime(): expected high 32 bits of T3 (0x%08X); got 0x%08X",
        (unsigned)(t3 >> 8), (unsigned)mock_delayedtrxtime_state.starttime);

    zassert_equal(mock_tx_state.writetxdata_called, 1,
        "expected dwt_writetxdata() called once; got %d",
        mock_tx_state.writetxdata_called);
    zassert_equal(mock_tx_state.writetxdata_len,
        (uint16)(UWB_TWR_RESPONSE_FRAME_SIZE + 2u),
        "writetxdata length must be the RESPONSE PSDU size + 2-byte CRC: "
        "expected %u; got %u",
        (unsigned)(UWB_TWR_RESPONSE_FRAME_SIZE + 2u),
        (unsigned)mock_tx_state.writetxdata_len);

    const uint8_t *resp = mock_tx_state.writetxdata_buf;

    zassert_equal(local_read_le16(&resp[UWB_OFF_DEST_ADDR]), initiator_addr,
        "RESPONSE dest_addr must be the initiator's address");
    zassert_equal(local_read_le16(&resp[UWB_OFF_SRC_ADDR]), self_addr,
        "RESPONSE src_addr must be this responder's address");
    zassert_equal(resp[UWB_OFF_FRAME_TYPE], (uint8_t)UWB_FRAME_TYPE_TWR_RESPONSE,
        "RESPONSE frame_type");
    zassert_equal(local_read_le16(&resp[UWB_OFF_TWR_EXCHANGE_ID]), exchange_id,
        "RESPONSE exchange_id must echo the POLL's exchange_id");
    zassert_equal(uwb_ts_read40(&resp[UWB_OFF_TWR_RESP_POLL_RX_TS]), t2,
        "RESPONSE poll_rx_ts (T2): expected 0x%llX, got 0x%llX",
        (unsigned long long)t2,
        (unsigned long long)uwb_ts_read40(&resp[UWB_OFF_TWR_RESP_POLL_RX_TS]));
    zassert_equal(uwb_ts_read40(&resp[UWB_OFF_TWR_RESP_RESP_TX_TS]), t3,
        "RESPONSE resp_tx_ts (T3): expected 0x%llX, got 0x%llX",
        (unsigned long long)t3,
        (unsigned long long)uwb_ts_read40(&resp[UWB_OFF_TWR_RESP_RESP_TX_TS]));

    zassert_equal(mock_starttx_state.called, 1,
        "expected dwt_starttx() called once; got %d", mock_starttx_state.called);
    zassert_true((mock_starttx_state.mode & (uint8_t)DWT_RESPONSE_EXPECTED) != 0,
        "dwt_starttx() mode must include DWT_RESPONSE_EXPECTED (0x%02X); got 0x%02X",
        (unsigned)DWT_RESPONSE_EXPECTED, (unsigned)mock_starttx_state.mode);
    zassert_true((mock_starttx_state.mode & (uint8_t)DWT_START_TX_DELAYED) != 0,
        "dwt_starttx() mode must include DWT_START_TX_DELAYED");

    /* --- FINAL capture: T1/T4/T5 extracted, T6 captured ------------------- */
    zassert_equal(out.initiator_addr, initiator_addr, "out.initiator_addr");
    zassert_equal(out.exchange_id, exchange_id, "out.exchange_id");
    zassert_equal(out.poll_tx_ts, t1, "out.poll_tx_ts (T1)");
    zassert_equal(out.poll_rx_ts, t2, "out.poll_rx_ts (T2)");
    zassert_equal(out.resp_tx_ts, t3, "out.resp_tx_ts (T3)");
    zassert_equal(out.resp_rx_ts, t4, "out.resp_rx_ts (T4)");
    zassert_equal(out.final_tx_ts, t5, "out.final_tx_ts (T5)");
    zassert_equal(out.final_rx_ts, t6, "out.final_rx_ts (T6)");

    /* --- Debug range: CONFIG_UWB_TWR_RESPONDER_DEBUG_RANGE=1 (injected in
     * CMakeLists.txt) -- 1000 ticks * 4.691763 mm/tick = 4691.763 mm,
     * truncates to 4691. Hand-calculated independently of the production
     * constant to avoid a tautological check; allow +/-1mm for any
     * double-rounding difference. --------------------------------------- */
    const int64_t expected_range_mm = 4691;
    int64_t diff = out.range_mm - expected_range_mm;

    if (diff < 0) {
        diff = -diff;
    }
    zassert_true(diff <= 1,
        "range_mm: expected ~%lld mm, got %lld mm",
        (long long)expected_range_mm, (long long)out.range_mm);
}

/* ---------------------------------------------------------------------------
 * Test 2 -- out == NULL does not crash the happy path
 * --------------------------------------------------------------------------- */
ZTEST(twr_responder, test_full_exchange_with_null_out_does_not_crash)
{
    const uint16_t self_addr = 0x0002;
    const uint16_t initiator_addr = 0x0001;
    const uint16_t exchange_id = 0x0042;
    const uint64_t t2 = 2000000ULL;
    const uint64_t t3 = dw1000_delayed_tx_time(t2, CONFIG_UWB_TWR_RESP_REPLY_DELAY_DTU);

    uint8_t poll_buf[UWB_TWR_POLL_FRAME_SIZE];
    uint8_t final_buf[UWB_TWR_FINAL_FRAME_SIZE];

    build_poll_frame(poll_buf, initiator_addr, self_addr, 0x01, exchange_id);
    push_good_frame(poll_buf, sizeof(poll_buf), t2);

    build_final_frame(final_buf, initiator_addr, self_addr, 0x02, exchange_id,
                       100ULL, t3 + 200ULL, t3 + 300ULL);
    push_good_frame(final_buf, sizeof(final_buf), t3 + 400ULL);

    twr_responder_status_t status = twr_responder_run_once(self_addr, NULL);

    zassert_equal(status, TWR_RESPONDER_EXCHANGE_OK,
        "expected TWR_RESPONDER_EXCHANGE_OK with out=NULL, got %d", (int)status);
}

/* ---------------------------------------------------------------------------
 * Test 3 -- no POLL heard before the RX timeout: stays armed, no error
 * --------------------------------------------------------------------------- */
ZTEST(twr_responder, test_no_poll_returns_no_poll_and_does_not_tx)
{
    /* No RX events pushed -- dwt_rxenable()'s first call falls through to
     * the mock's default frame-wait-timeout event. */
    uwb_twr_exchange_t out;

    twr_responder_status_t status = twr_responder_run_once(0x0002, &out);

    zassert_equal(status, TWR_RESPONDER_NO_POLL,
        "expected TWR_RESPONDER_NO_POLL, got %d", (int)status);
    zassert_equal(mock_rxenable_state.called, 1,
        "expected exactly one dw1000_rx() attempt; got %d",
        mock_rxenable_state.called);
    zassert_equal(mock_delayedtrxtime_state.called, 0,
        "no RESPONSE should be scheduled when no POLL was heard");
    zassert_equal(mock_tx_state.writetxdata_called, 0,
        "no RESPONSE should be built/sent when no POLL was heard");

    /* out must be left zeroed (memset at function entry). */
    zassert_equal(out.exchange_id, 0, "out must be zeroed on TWR_RESPONDER_NO_POLL");
    zassert_equal(out.poll_rx_ts, 0, "out must be zeroed on TWR_RESPONDER_NO_POLL");
}

/* ---------------------------------------------------------------------------
 * Test 4 -- POLL heard but addressed to a different short address: ignored
 * --------------------------------------------------------------------------- */
ZTEST(twr_responder, test_poll_wrong_dest_addr_is_foreign_frame)
{
    uint8_t poll_buf[UWB_TWR_POLL_FRAME_SIZE];

    /* Addressed to 0x0099, but we run the responder as 0x0002. */
    build_poll_frame(poll_buf, 0x0001, 0x0099, 0x01, 0x5555);
    push_good_frame(poll_buf, sizeof(poll_buf), 12345ULL);

    twr_responder_status_t status = twr_responder_run_once(0x0002, NULL);

    zassert_equal(status, TWR_RESPONDER_FOREIGN_FRAME,
        "expected TWR_RESPONDER_FOREIGN_FRAME for wrong dest_addr, got %d",
        (int)status);
    zassert_equal(mock_rxenable_state.called, 1,
        "expected exactly one dw1000_rx() attempt; got %d",
        mock_rxenable_state.called);
    zassert_equal(mock_delayedtrxtime_state.called, 0,
        "no RESPONSE should be scheduled for a POLL addressed elsewhere");
}

/* ---------------------------------------------------------------------------
 * Test 5 -- malformed / wrong-frame-type frame heard while waiting for POLL
 * --------------------------------------------------------------------------- */
ZTEST(twr_responder, test_wrong_frame_type_while_waiting_for_poll_is_foreign_frame)
{
    uint8_t poll_buf[UWB_TWR_POLL_FRAME_SIZE];

    build_poll_frame(poll_buf, 0x0001, 0x0002, 0x01, 0x5555);
    /* Corrupt the frame_type so uwb_parse_twr_poll() rejects it. */
    poll_buf[UWB_OFF_FRAME_TYPE] = (uint8_t)UWB_FRAME_TYPE_TWR_RESPONSE;
    push_good_frame(poll_buf, sizeof(poll_buf), 12345ULL);

    twr_responder_status_t status = twr_responder_run_once(0x0002, NULL);

    zassert_equal(status, TWR_RESPONDER_FOREIGN_FRAME,
        "expected TWR_RESPONDER_FOREIGN_FRAME for a malformed POLL, got %d",
        (int)status);
    zassert_equal(mock_delayedtrxtime_state.called, 0,
        "no RESPONSE should be scheduled for a malformed POLL");
}

/* ---------------------------------------------------------------------------
 * Test 6 -- a valid POLL is heard but dw1000_tx_at() misses the scheduled
 * slot (HPDWARN): abort, no FINAL wait attempted
 * --------------------------------------------------------------------------- */
ZTEST(twr_responder, test_response_tx_failure_aborts_without_waiting_for_final)
{
    uint8_t poll_buf[UWB_TWR_POLL_FRAME_SIZE];

    build_poll_frame(poll_buf, 0x0001, 0x0002, 0x01, 0x7777);
    push_good_frame(poll_buf, sizeof(poll_buf), 999ULL);

    mock_starttx_state.return_value = DWT_ERROR;

    twr_responder_status_t status = twr_responder_run_once(0x0002, NULL);

    zassert_equal(status, TWR_RESPONDER_TX_ERROR,
        "expected TWR_RESPONDER_TX_ERROR, got %d", (int)status);
    zassert_equal(mock_starttx_state.called, 1,
        "expected dwt_starttx() called once; got %d", mock_starttx_state.called);
    zassert_equal(mock_rxenable_state.called, 1,
        "no FINAL wait should be attempted after a failed RESPONSE tx; "
        "expected exactly one dw1000_rx() attempt, got %d",
        mock_rxenable_state.called);
}

/* ---------------------------------------------------------------------------
 * Test 7 -- POLL answered, but no FINAL heard before the RX timeout
 * --------------------------------------------------------------------------- */
ZTEST(twr_responder, test_no_final_after_response_returns_no_final)
{
    uint8_t poll_buf[UWB_TWR_POLL_FRAME_SIZE];

    build_poll_frame(poll_buf, 0x0001, 0x0002, 0x01, 0x8888);
    push_good_frame(poll_buf, sizeof(poll_buf), 555ULL);
    /* No second event pushed -- the FINAL-wait dw1000_rx() call falls
     * through to the mock's default frame-wait-timeout event. */

    twr_responder_status_t status = twr_responder_run_once(0x0002, NULL);

    zassert_equal(status, TWR_RESPONDER_NO_FINAL,
        "expected TWR_RESPONDER_NO_FINAL, got %d", (int)status);
    zassert_equal(mock_rxenable_state.called, 2,
        "expected two dw1000_rx() attempts (POLL wait + FINAL wait); got %d",
        mock_rxenable_state.called);
    zassert_equal(mock_tx_state.writetxdata_called, 1,
        "RESPONSE must still have been sent before the FINAL wait; got %d",
        mock_tx_state.writetxdata_called);
}

/* ---------------------------------------------------------------------------
 * Test 8 -- a FINAL is heard, but its exchange_id does not match the
 * pending exchange: ignored
 * --------------------------------------------------------------------------- */
ZTEST(twr_responder, test_final_exchange_id_mismatch_is_ignored)
{
    const uint16_t exchange_id = 0x1111;
    const uint16_t wrong_exchange_id = 0x2222;

    uint8_t poll_buf[UWB_TWR_POLL_FRAME_SIZE];
    uint8_t final_buf[UWB_TWR_FINAL_FRAME_SIZE];

    build_poll_frame(poll_buf, 0x0001, 0x0002, 0x01, exchange_id);
    push_good_frame(poll_buf, sizeof(poll_buf), 111ULL);

    build_final_frame(final_buf, 0x0001, 0x0002, 0x02, wrong_exchange_id,
                       10ULL, 20ULL, 30ULL);
    push_good_frame(final_buf, sizeof(final_buf), 40ULL);

    twr_responder_status_t status = twr_responder_run_once(0x0002, NULL);

    zassert_equal(status, TWR_RESPONDER_FINAL_MISMATCH,
        "expected TWR_RESPONDER_FINAL_MISMATCH, got %d", (int)status);
}

/* ---------------------------------------------------------------------------
 * Test 9 -- uwb_twr_range_mm(): pure function, hand-calc fixture
 * --------------------------------------------------------------------------- */
ZTEST(twr_responder, test_range_mm_hand_calc_fixture)
{
    /* T_round1 = T_round2 = 3000, T_reply1 = T_reply2 = 1000 ->
     * ToF = (3000*3000 - 1000*1000) / (3000+3000+1000+1000)
     *     = (9,000,000 - 1,000,000) / 8000 = 1000 ticks
     *     -> 1000 * 4.691763 mm/tick = 4691.763 mm -> truncates to 4691. */
    const uint64_t t1 = 10000ULL;
    const uint64_t t2 = 10300ULL;
    const uint64_t t3 = 11300ULL;
    const uint64_t t4 = 13000ULL;
    const uint64_t t5 = 14000ULL;
    const uint64_t t6 = 14300ULL;

    int64_t range_mm = uwb_twr_range_mm(t1, t2, t3, t4, t5, t6);
    int64_t diff = range_mm - 4691;

    if (diff < 0) {
        diff = -diff;
    }
    zassert_true(diff <= 1,
        "expected range_mm ~= 4691, got %lld", (long long)range_mm);
}

ZTEST(twr_responder, test_range_mm_degenerate_zero_denominator)
{
    /* All six timestamps equal -> every T_round/T_reply delta is 0 ->
     * denominator 0 -> the function must return 0, not divide by zero. */
    int64_t range_mm = uwb_twr_range_mm(42ULL, 42ULL, 42ULL, 42ULL, 42ULL, 42ULL);

    zassert_equal(range_mm, 0,
        "expected 0 for a degenerate (zero-denominator) input, got %lld",
        (long long)range_mm);
}
