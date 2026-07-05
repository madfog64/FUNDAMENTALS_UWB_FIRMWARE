/*! ----------------------------------------------------------------------------
 * @file    dw1000_config.h
 * @brief   DW1000 PHY radio configuration layer (UWB-155, UWB-156).
 *
 * Exposes dw1000_configure() and the antenna-delay set/get API.
 *
 * dw1000_configure() executes the complete PHY bring-up sequence:
 *
 *   1. reset_DW1000()              hardware reset via RESET GPIO
 *   2. port_set_dw1000_slowrate()  SPI < 3 MHz (DW1000 requirement)
 *   3. dwt_initialise(DWT_LOADUCODE)   LDE microcode load for accurate RX TS
 *   4. port_set_dw1000_fastrate()  raise SPI to 8 MHz
 *   5. dwt_configure(&cfg)         PHY params (channel/PRF/PLEN/PAC/codes/
 *                                              SFD/data-rate/PHR mode/sfdTO)
 *   6. dwt_configuretxrf(&txcfg)   PG delay + TX power for channel 5
 *   7. dwt_settxantennadelay() +   antenna delay: OTP when present, else
 *      dwt_setrxantennadelay()     CONFIG_DW1000_ANTENNA_DELAY_{TX,RX}
 *
 * PHY parameters are sourced from Kconfig symbols (CONFIG_DW1000_PHY_*) whose
 * defaults are set to exactly match contracts/uwb/phy_config.h version 0.6:
 *
 *   Symbol                    Default    contracts/uwb constant
 *   --------------------------+----------+---------------------------------
 *   CONFIG_DW1000_PHY_CHANNEL   5          UWB_PHY_CHANNEL
 *   CONFIG_DW1000_PHY_PRF       2          UWB_PHY_PRF       (DWT_PRF_64M)
 *   CONFIG_DW1000_PHY_TX_PLEN   0x04       UWB_PHY_TX_PLEN   (DWT_PLEN_64)
 *   CONFIG_DW1000_PHY_PAC       0          UWB_PHY_PAC       (DWT_PAC8)
 *   CONFIG_DW1000_PHY_TX_CODE   9          UWB_PHY_TX_CODE
 *   CONFIG_DW1000_PHY_RX_CODE   9          UWB_PHY_RX_CODE
 *   CONFIG_DW1000_PHY_NS_SFD    0          UWB_PHY_NS_SFD    (standard SFD)
 *   CONFIG_DW1000_PHY_DATA_RATE 2          UWB_PHY_DATA_RATE (DWT_BR_6M8)
 *   CONFIG_DW1000_PHY_PHR_MODE  0          UWB_PHY_PHR_MODE  (DWT_PHRMODE_STD)
 *   CONFIG_DW1000_PHY_SFD_TIMEOUT 65       UWB_PHY_SFD_TIMEOUT
 *   CONFIG_DW1000_PHY_TX_PGDELAY 0xC0      UWB_PHY_TX_PGDELAY (TC_PGDELAY_CH5)
 *   CONFIG_DW1000_PHY_TX_POWER  0x0E082848 UWB_PHY_TX_POWER_NOMINAL (dev only)
 *
 * SFD timeout formula (DW1000 User Manual):
 *   sfdTO = PLEN(64) + 1 + SFD_len(8) - PAC(8) = 65  (canonical value)
 *
 * TX power note:
 *   CONFIG_DW1000_PHY_TX_POWER defaults to UWB_PHY_TX_POWER_NOMINAL
 *   (0x0E082848 — non-boosted, channel 5, PRF 64 MHz, smart TX disabled).
 *   This is suitable for blank development hardware only.  Production hardware
 *   MUST replace this with the OTP-programmed factory TX power read via
 *   dwt_read32bitreg(TX_POWER_ID) after dwt_initialise() completes.  Factory
 *   calibration is required for regulatory compliance.
 *
 * Call sequence:
 *   dw1000_port_init();      // one-time port setup (from deca_port.h)
 *   dw1000_configure();      // this module — PHY bring-up and config
 *   // DW1000 is now ready for TX/RX operations.
 *
 * Both functions must be called from a thread context (not ISR) because
 * they issue SPI transactions via the Zephyr SPI subsystem.
 *
 * Contracts pinning:
 *   This module consumes drivers/dw1000/phy_config.h, which is a pinned copy
 *   of contracts/uwb/phy_config.h.  The pinned version is recorded in
 *   CONTRACTS_VERSION at the repo root.  Both Kconfig defaults and compile-time
 *   assertions cross-check that the constants match.
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef DW1000_CONFIG_H_
#define DW1000_CONFIG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief OTP address of the factory antenna-delay word (DW1000-specific).
 *
 * The 32-bit word at this OTP address packs two 16-bit antenna-delay values:
 *   bits[15:0]  — PRF 16 MHz antenna delay (ticks)
 *   bits[31:16] — PRF 64 MHz antenna delay (ticks)
 *
 * dw1000_configure() reads this word via dwt_otpread().  The PRF-64 half
 * (matching the canonical CONFIG_DW1000_PHY_PRF = 2 = DWT_PRF_64M) is
 * extracted.  A non-zero value overrides CONFIG_DW1000_ANTENNA_DELAY_{TX,RX}.
 *
 * Reference: deca_device.c ANTDLY_ADDRESS = 0x1C (same address used by the
 * prior-generation nRF5 SDK firmware's read_antenna_delay_from_otp()).
 */
#define DW1000_OTP_ANTDLY_ADDRESS   0x1CU

/**
 * @brief Execute the full DW1000 PHY configuration sequence.
 *
 * Resets the DW1000, loads the LDE microcode (DWT_LOADUCODE), then applies
 * the radio PHY parameters defined by CONFIG_DW1000_PHY_* (channel, PRF,
 * preamble length, PAC, codes, SFD, data rate, PHR mode, SFD timeout),
 * configures the TX power profile, and programs the antenna delays.
 *
 * Antenna delay precedence (step 7):
 *   1. OTP word at DW1000_OTP_ANTDLY_ADDRESS, PRF-64 half — if non-zero.
 *   2. CONFIG_DW1000_ANTENNA_DELAY_TX / CONFIG_DW1000_ANTENNA_DELAY_RX — fallback.
 *
 * The effective delay is logged at DBG level and is readable via
 * dw1000_get_antenna_delay().
 *
 * Must be called AFTER dw1000_port_init() and from a thread context.
 *
 * @return  0        Success; DW1000 is ready for TX/RX.
 * @return  -EIO     dwt_initialise(DWT_LOADUCODE) failed (SPI fault or
 *                   device not detected — check wiring and slow SPI rate).
 */
int dw1000_configure(void);

/**
 * @brief Override the TX and RX antenna delays at runtime.
 *
 * Programs @p tx_delay and @p rx_delay into the DW1000 hardware registers
 * (TX_ANTD and LDE_RXANTD) via dwt_settxantennadelay() /
 * dwt_setrxantennadelay(), and caches the values for dw1000_get_antenna_delay().
 *
 * Call this AFTER dw1000_configure() (which establishes the initial values).
 * Intended for antenna-delay calibration workflows (ADR-001 open item) — not
 * for normal runtime use.
 *
 * @param tx_delay  TX antenna delay in DW1000 ticks (≈ 15.65 ps / tick).
 * @param rx_delay  RX antenna delay in DW1000 ticks (≈ 15.65 ps / tick).
 */
void dw1000_set_antenna_delay(uint16_t tx_delay, uint16_t rx_delay);

/**
 * @brief Read back the effective TX and RX antenna delays.
 *
 * Returns the values last programmed by dw1000_configure() or
 * dw1000_set_antenna_delay().  Both pointers may be NULL if the caller only
 * needs one of the two values.
 *
 * @param[out] tx_delay  Set to the current TX antenna delay, or ignored if NULL.
 * @param[out] rx_delay  Set to the current RX antenna delay, or ignored if NULL.
 */
void dw1000_get_antenna_delay(uint16_t *tx_delay, uint16_t *rx_delay);

#ifdef __cplusplus
}
#endif

#endif /* DW1000_CONFIG_H_ */
