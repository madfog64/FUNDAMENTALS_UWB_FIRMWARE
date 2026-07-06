/*! ----------------------------------------------------------------------------
 * @file    dw1000_ranging.h
 * @brief   DW1000 ranging radio primitives: RX-with-timestamp and scheduled
 *          (delayed) TX (UWB-231).
 *
 * Thin, reusable layer over the vendored deca_driver for the two ranging
 * primitives every TWR/TDoA role needs:
 *
 *   - dw1000_rx()                  polled receive with hardware RX timestamp
 *   - dw1000_tx_at()                schedule a delayed TX at an absolute
 *                                   DW1000 system time, optionally arming the
 *                                   receiver afterwards (DS-TWR poll -> resp)
 *   - dw1000_delayed_tx_time()      compute a correctly truncated delayed TX
 *                                   time from a reference timestamp + a reply
 *                                   delay
 *   - dw1000_read_rx_timestamp() /
 *     dw1000_read_tx_timestamp()    40-bit timestamp helpers
 *
 * PHY + antenna delay are already configured by dw1000_configure() (UWB-7,
 * UWB-155, UWB-156) before any of these functions are called — every
 * timestamp returned here is therefore already antenna-delay corrected by the
 * DW1000 hardware (TX_ANTD / LDE_RXANTD registers).  This module only adds
 * the RX/TX timing operations.
 *
 * Out of scope (see other UWB-8 subissues):
 *   - Ranging frame build/parse (UWB-230).
 *   - The TWR responder state machine + sample application (UWB-232).
 *   - Interrupt-driven RX — this module is polled only.
 *
 * -----------------------------------------------------------------------
 * On-hardware bring-up (manual — not automated by the ztest suite)
 * -----------------------------------------------------------------------
 * The ztest suite (tests/dw1000_ranging/) exercises this module entirely
 * against a mocked deca_driver on the 'unit_testing' platform; it cannot
 * verify real RF timing.  Before relying on this module for TWR/TDoA on a
 * DWM1001, validate on real hardware:
 *
 *   1. Two DWM1001 boards, PHY-configured via dw1000_configure().
 *   2. Board A: dw1000_tx_at() with tx_time_dtu = dw1000_delayed_tx_time(
 *      dwt_readsystimestamphi32() << 8, <a few ms of margin in DTU>) — i.e.
 *      schedule a TX comfortably in the future relative to "now" so the slot
 *      is not missed. Confirm it returns 0 (not -EIO/HPDWARN).
 *   3. Board B: dw1000_rx() with a generous timeout_us; confirm it returns 0
 *      with the expected payload and a plausible rx_ts (compare against
 *      Board A's dw1000_read_tx_timestamp() taken after its TX call — the
 *      difference should correspond to the real propagation + processing
 *      time, not a garbage/zero value).
 *   4. Repeat with expect_response = true on Board A and confirm the
 *      receiver comes up automatically (no explicit dw1000_rx() call needed
 *      to observe RX activity) — inspect via a logic analyser on the DW1000
 *      IRQ line or RX LED if available.
 *   5. Deliberately schedule a tx_time_dtu that has already passed (e.g. use
 *      a stale/old anchor_ts) and confirm dw1000_tx_at() returns -EIO
 *      (HPDWARN) rather than silently transmitting late.
 *
 *   Record board IDs, firmware git SHA, and pass/fail per step in the PR or a
 *   bring-up log — this module's PR is build-verified only, not
 *   hardware-verified, until this checklist has been run.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef DW1000_RANGING_H_
#define DW1000_RANGING_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Polled receive: arm the receiver, wait for a frame, and read back
 *        its hardware RX timestamp.
 *
 * Sequence: dwt_setrxtimeout() + dwt_rxenable(DWT_START_RX_IMMEDIATE), then
 * busy-polls SYS_STATUS until either a good frame (RXFCG), an RX error
 * (PHY header / FCS / Reed-Solomon / SFD timeout — SYS_STATUS_ALL_RX_ERR),
 * or an RX timeout (frame-wait or preamble-detect — SYS_STATUS_ALL_RX_TO) is
 * latched.  The DW1000 hardware RX timeout (derived from @p timeout_us)
 * bounds the poll loop, so this function always returns on real hardware.
 *
 * On a good frame, reads back RX_FINFO for the received frame length, copies
 * the payload into @p buf (bounded by the caller-supplied buffer capacity
 * passed in via *len on entry), and assembles the 40-bit RX timestamp.
 *
 * @param[out]    buf       Buffer to receive the frame payload.
 * @param[in,out] len       In: capacity of @p buf, in bytes.
 *                          Out: number of bytes written to @p buf on success
 *                          (0 on any error).
 * @param[out]    rx_ts     Set to the 40-bit RX timestamp (DW1000 ticks,
 *                          already antenna-delay corrected) on success.
 * @param timeout_us        Hardware RX timeout, in microseconds, converted to
 *                          DW1000 RX_FWTO units (~1.0256 us/unit, DW1000 User
 *                          Manual sec 7.2.40). 0 disables the hardware
 *                          timeout (receiver stays armed indefinitely — use
 *                          with care; every real caller should pass a
 *                          non-zero bound).
 *
 * @return  0          Good frame received; @p buf / @p len / @p rx_ts valid.
 * @return  -EINVAL    NULL buf/len/rx_ts, or the received frame is larger
 *                      than the caller's buffer capacity (*len on entry).
 * @return  -ETIMEDOUT No frame received before the RX timeout expired.
 * @return  -EIO       RX error latched (FCS / PHY header / Reed-Solomon /
 *                      SFD timeout), or dwt_rxenable() itself failed.
 */
int dw1000_rx(uint8_t *buf, uint16_t *len, uint64_t *rx_ts, uint32_t timeout_us);

/**
 * @brief Read back the 40-bit hardware RX timestamp of the last received
 *        frame (already antenna-delay corrected by LDE_RXANTD).
 *
 * Wraps dwt_readrxtimestamp()'s 5-byte little-endian buffer into a uint64_t.
 * Normally you do not need to call this directly — dw1000_rx() already
 * returns the RX timestamp for the frame it received.
 *
 * @return  40-bit RX timestamp, DW1000 ticks (bits above 39 always zero).
 */
uint64_t dw1000_read_rx_timestamp(void);

/**
 * @brief Read back the 40-bit hardware TX timestamp of the last transmitted
 *        frame (already antenna-delay corrected by TX_ANTD).
 *
 * Wraps dwt_readtxtimestamp()'s 5-byte little-endian buffer into a uint64_t.
 * Call this after dw1000_tx_at() reports success (it already waits for
 * TXFRS — frame sent — before returning, so the timestamp is ready).
 *
 * @return  40-bit TX timestamp, DW1000 ticks (bits above 39 always zero).
 */
uint64_t dw1000_read_tx_timestamp(void);

/**
 * @brief Schedule a deterministic delayed transmission at an absolute
 *        DW1000 system time, then wait for it to complete.
 *
 * Sequence: dwt_setdelayedtrxtime(), dwt_writetxdata(), dwt_writetxfctrl()
 * (ranging bit always set — every frame this module transmits is a TWR/TDoA
 * ranging frame), dwt_starttx(DWT_START_TX_DELAYED [| DWT_RESPONSE_EXPECTED]).
 * Polls SYS_STATUS for TXFRS (frame sent) once dwt_starttx() reports success.
 *
 * @p len follows deca_driver convention: it is the *total* frame length
 * including the 2-byte CRC that the DW1000 hardware appends automatically
 * (@p buf itself must NOT contain the CRC bytes).
 *
 * Timing / antenna-delay note:
 *   dwt_setdelayedtrxtime() only accepts the HIGH 32 bits of the 40-bit
 *   DX_TIME register (bits [39:8]) — this function passes
 *   (uint32)(tx_time_dtu >> 8).  The DW1000 hardware additionally ignores
 *   the low 9 bits of the full 40-bit time when it decides the actual TX
 *   instant (DW1000 User Manual sec 3.3) — i.e. the true schedulable
 *   granularity is 2^9 = 512 ticks (~8.01 ns), one bit finer than the 2^8
 *   truncation the 32-bit API argument alone would suggest. Callers should
 *   pass a @p tx_time_dtu already produced by dw1000_delayed_tx_time()
 *   (which performs this truncation up front) rather than an arbitrary raw
 *   value, so the value they later compare against the reported TX
 *   timestamp is the one that was actually honoured.
 *
 *   The *physical* antenna departure time is the programmed (truncated)
 *   time PLUS the TX antenna delay (TX_ANTD, programmed by
 *   dw1000_configure()) — the same offset the hardware already adds when it
 *   reports the TX timestamp via dwt_readtxtimestamp() /
 *   dw1000_read_tx_timestamp(). No extra correction is needed by callers of
 *   this module; the antenna delay is purely a reporting/interop concern,
 *   not something this function needs to add to @p tx_time_dtu itself.
 *
 * @param buf              Frame bytes to transmit (PSDU, no length octet,
 *                          no CRC — see @p len note above).
 * @param len              Frame length in bytes, including the 2-byte CRC
 *                          (deca_driver convention).
 * @param tx_time_dtu      Absolute 40-bit DW1000 system time (ticks) at
 *                          which to transmit; typically the output of
 *                          dw1000_delayed_tx_time().
 * @param expect_response  true to also set DWT_RESPONSE_EXPECTED, arming the
 *                          receiver automatically after the delayed TX
 *                          completes (DS-TWR poll -> expect response).
 *
 * @return  0        Frame scheduled and TXFRS latched (transmission complete).
 * @return  -EINVAL  NULL buf, zero len, or dwt_writetxdata() rejected the
 *                    frame (e.g. length/offset out of range).
 * @return  -EIO     dwt_starttx() reported an error — the scheduled slot was
 *                    missed (HPDWARN latched in SYS_STATUS, i.e. the
 *                    requested @p tx_time_dtu had already passed by the time
 *                    dwt_starttx() ran). Callers must abort the exchange
 *                    rather than retry blindly, since the peer will not have
 *                    seen a transmission at the expected time.
 */
int dw1000_tx_at(const uint8_t *buf, uint16_t len, uint64_t tx_time_dtu,
                  bool expect_response);

/**
 * @brief Compute a correctly truncated absolute delayed-TX time from a
 *        reference timestamp and a reply delay.
 *
 * Returns (anchor_ts + delay_dtu), masked down to a multiple of 512 (2^9)
 * ticks — the granularity the DW1000 hardware actually honours for a
 * delayed TX/RX start time (see dw1000_tx_at() doc comment above). The
 * result wraps modulo 2^40, matching the DW1000's 40-bit system time
 * counter.
 *
 * @param anchor_ts   Reference 40-bit timestamp (e.g. the RX timestamp of a
 *                    received poll message, "T2" in DS-TWR terms).
 * @param delay_dtu   Reply delay to add, in DW1000 time units — e.g.
 *                    CONFIG_UWB_TWR_RESP_REPLY_DELAY_DTU for a TWR responder
 *                    computing T3 = T2 + delay.
 *
 * @return  The 40-bit delayed TX time, low 9 bits zeroed, ready to pass as
 *          @p tx_time_dtu to dw1000_tx_at().
 */
uint64_t dw1000_delayed_tx_time(uint64_t anchor_ts, uint32_t delay_dtu);

#ifdef __cplusplus
}
#endif

#endif /* DW1000_RANGING_H_ */
