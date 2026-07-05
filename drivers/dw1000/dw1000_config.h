/*! ----------------------------------------------------------------------------
 * @file    dw1000_config.h
 * @brief   DW1000 PHY radio configuration layer (UWB-155).
 *
 * Exposes a single dw1000_configure() entry point that executes the complete
 * PHY bring-up sequence required before any UWB TX/RX operation:
 *
 *   1. reset_DW1000()              hardware reset via RESET GPIO
 *   2. port_set_dw1000_slowrate()  SPI < 3 MHz (DW1000 requirement)
 *   3. dwt_initialise(DWT_LOADUCODE)   LDE microcode load for accurate RX TS
 *   4. port_set_dw1000_fastrate()  raise SPI to 8 MHz
 *   5. dwt_configure(&cfg)         PHY params (channel/PRF/PLEN/PAC/codes/
 *                                              SFD/data-rate/PHR mode/sfdTO)
 *   6. dwt_configuretxrf(&txcfg)   PG delay + TX power for channel 5
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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Execute the full DW1000 PHY configuration sequence.
 *
 * Resets the DW1000, loads the LDE microcode (DWT_LOADUCODE), then applies
 * the radio PHY parameters defined by CONFIG_DW1000_PHY_* (channel, PRF,
 * preamble length, PAC, codes, SFD, data rate, PHR mode, SFD timeout) and
 * configures the TX power profile.
 *
 * Must be called AFTER dw1000_port_init() and from a thread context.
 *
 * @return  0        Success; DW1000 is ready for TX/RX.
 * @return  -EIO     dwt_initialise(DWT_LOADUCODE) failed (SPI fault or
 *                   device not detected — check wiring and slow SPI rate).
 */
int dw1000_configure(void);

#ifdef __cplusplus
}
#endif

#endif /* DW1000_CONFIG_H_ */
