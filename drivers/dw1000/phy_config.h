/*
 * phy_config.h — Canonical DW1000 PHY configuration
 *
 * Single source of truth for the DW1000 radio parameters shared between:
 *
 *   Tag firmware   (FUNDAMENTALS_UWB_FIRMWARE, C / Zephyr / nRF Connect SDK)
 *   Anchor host    (FUNDAMENTALS_ANCHOR, C++ / Raspberry Pi Zero W)
 *
 * Both consumers MUST load this exact configuration.  Silent divergence on
 * any parameter produces inter-operability failures that are hard to diagnose
 * in the field (missed receptions, no ranging, wrong timing).
 *
 * This header is self-contained: it does NOT include deca_device_api.h.
 * The numeric values match the corresponding DWT_* constants defined in that
 * file; the equivalence is documented per constant so that readers can
 * cross-check without opening the driver header.
 *
 * Usage in firmware / anchor C/C++ code:
 *
 *   #include "deca_device_api.h"
 *   #include "phy_config.h"
 *
 *   static dwt_config_t uwb_phy = {
 *       UWB_PHY_CHANNEL,        // chan
 *       UWB_PHY_PRF,            // prf
 *       UWB_PHY_TX_PLEN,        // txPreambLength
 *       UWB_PHY_PAC,            // rxPAC
 *       UWB_PHY_TX_CODE,        // txCode
 *       UWB_PHY_RX_CODE,        // rxCode
 *       UWB_PHY_NS_SFD,         // nsSFD
 *       UWB_PHY_DATA_RATE,      // dataRate
 *       UWB_PHY_PHR_MODE,       // phrMode
 *       UWB_PHY_SFD_TIMEOUT     // sfdTO
 *   };
 *
 *   static dwt_txconfig_t uwb_txcfg = {
 *       UWB_PHY_TX_PGDELAY,
 *       0  // production: replace with dwt_readtxpower() OTP value
 *   };
 *
 * Cross-repo event: any change to this header is a cross-repo event.
 * Firmware and anchor repos must both update simultaneously.  The change
 * requires a MINOR version bump (see contracts/uwb/README.md §Versioning).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef UWB_PHY_CONFIG_H
#define UWB_PHY_CONFIG_H

/* C++ guard — consumable by both C (firmware) and C++ (anchor host) */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* =========================================================================
 * PHY config file version
 *
 * Tracks the UWB_PROTOCOL_VERSION of contracts/uwb/ at the time this file
 * was introduced.  Consumers that pin an older contracts/uwb/ release can
 * detect a mismatch at compile time:
 *
 *   #if UWB_PHY_CONFIG_VERSION_MAJOR != 0 || UWB_PHY_CONFIG_VERSION_MINOR != 6
 *   #  error "Unexpected phy_config.h version — re-pin contracts/uwb/"
 *   #endif
 * ========================================================================= */
#define UWB_PHY_CONFIG_VERSION_MAJOR  0
#define UWB_PHY_CONFIG_VERSION_MINOR  6
#define UWB_PHY_CONFIG_VERSION        "0.6"

/* =========================================================================
 * Channel
 *
 * Channel 5 — centre frequency 6489.6 MHz, bandwidth 499.2 MHz.
 *
 * Rationale:
 *   Channel 5 is the most widely tested and best-characterised DW1000
 *   channel in peer-reviewed ranging literature and in Decawave/Qorvo
 *   reference designs.  Its centre frequency (6.5 GHz vs channels 1-4 at
 *   3-4 GHz) provides shorter wavelength and marginally better multipath
 *   resolution in the indoor environments where this system operates.
 *   Channel 5 has solid regulatory approval in the US (FCC Part 15.519),
 *   the EU (ETSI EN 302 065-1), and most of Asia-Pacific.
 *
 *   Channel 5 with PRF 64 MHz supports preamble codes 9-12 (standard IEEE
 *   802.15.4a).  These four codes are orthogonal, allowing future
 *   multi-network coexistence on adjacent courts.
 *
 *   The prior-generation system used channel 2 (3993.6 MHz, codes 9/9,
 *   850 kbps) — that configuration is incompatible with the airtime budget
 *   and is explicitly retired.  Do not revert to channel 2.
 * ========================================================================= */
#define UWB_PHY_CHANNEL              5u   /* {1, 2, 3, 4, 5, 7} are the valid DW1000 channels */

/* =========================================================================
 * Pulse Repetition Frequency (PRF)
 *
 * PRF 64 MHz (DWT_PRF_64M = 2).
 *
 * Rationale:
 *   PRF 64 MHz provides superior first-path detection in NLOS and
 *   multipath-heavy indoor environments (sports halls, gymnasiums) because
 *   its higher spreading gain allows the LDE (Leading-edge Detection)
 *   algorithm to isolate the leading edge of the first path from later
 *   multipath arrivals.  Better first-path isolation is the primary driver
 *   of TDoA accuracy in the absence of line-of-sight.
 *
 *   PRF 64 MHz is required for channel 5 preamble codes 9-12 (the standard
 *   channel 5 code set).  PRF 16 MHz on channel 5 uses codes 3-4, which
 *   are not selected here.
 * ========================================================================= */
#define UWB_PHY_PRF                  2u   /* DWT_PRF_64M = 2; DWT_PRF_16M = 1 */

/* =========================================================================
 * Preamble length — TX (DWT_PLEN_64 = 0x04)
 *
 * 64 symbols — standard IEEE 802.15.4a preamble length.
 *
 * Rationale and budget reconciliation:
 *   The contracts/uwb/README.md airtime tables target <= 150 us total per
 *   blink (approx 0.5 ms slot at 75 Hz, 24 tags).  At PRF 64 MHz each
 *   preamble symbol occupies approx 1.0 us:
 *
 *     PLEN  64:  approx 64 us preamble + 8 us SFD + 22 us PHR + 18 us data
 *               = approx 112 us total  -- within the 150 us budget.
 *
 *     PLEN 128:  approx 128 us + 8 us + 22 us + 18 us = approx 176 us total
 *               -- exceeds the 150 us budget by approx 26 us.
 *
 *   The 22 us PHR duration assumes 850 kbps.  The DW1000 hardware always
 *   transmits the PHR at 850 kbps regardless of the data-payload rate --
 *   this is a DW1000 hardware characteristic, not a configurable setting.
 *
 *   PLEN 64 is a standard preamble length (unlike PLEN 128 which is
 *   non-standard in DW1000 terms).  It provides reliable preamble
 *   acquisition for indoor ranges up to approx 30 m LOS and approx 15 m NLOS.
 *
 *   The prior-generation firmware used PLEN 128 (DWT_PLEN_128 = 0x14,
 *   non-standard), which was internally inconsistent with the 150 us budget.
 *   PLEN 64 corrects this and is also a standard length.
 * ========================================================================= */
#define UWB_PHY_TX_PLEN              0x04u /* DWT_PLEN_64 = 0x04; DWT_PLEN_128 (non-std) = 0x14 */

/* =========================================================================
 * Preamble Acquisition Chunk (PAC) size — RX (DWT_PAC8 = 0)
 *
 * PAC 8 symbols.
 *
 * Rationale:
 *   The DW1000 User Manual specifies PAC sizes as a function of TX preamble
 *   length:
 *
 *     PLEN <= 128  =>  PAC  8 (DWT_PAC8  = 0)  <-- this configuration
 *     PLEN  = 256  =>  PAC 16 (DWT_PAC16 = 1)
 *     PLEN  = 512  =>  PAC 32 (DWT_PAC32 = 2)
 *     PLEN >= 1024 =>  PAC 64 (DWT_PAC64 = 3)
 *
 *   PAC 8 is the only correct value for PLEN 64.  A larger PAC with a
 *   short preamble degrades acquisition sensitivity; PAC 8 is the minimum.
 * ========================================================================= */
#define UWB_PHY_PAC                  0u   /* DWT_PAC8 = 0 -- required for PLEN 64 */

/* =========================================================================
 * Preamble codes — TX and RX
 *
 * TX code 9, RX code 9.
 *
 * Rationale:
 *   For channel 5 + PRF 64 MHz, the DW1000 supports preamble codes 9, 10,
 *   11, and 12 (standard IEEE 802.15.4a).  Code 9 is the default in
 *   Decawave/Qorvo reference designs for this channel/PRF combination and
 *   the most widely characterised in ranging deployments.
 *
 *   TX and RX codes MUST match across all devices in the network (all tags
 *   and all anchors).  A device transmitting on a different preamble code
 *   will not be detected by receivers configured for code 9.
 *
 *   To switch codes (for multi-network coexistence): change both constants
 *   to the same new value and increment UWB_PROTOCOL_VERSION_MINOR in
 *   uwb_frames.h.
 * ========================================================================= */
#define UWB_PHY_TX_CODE              9u   /* Channel 5, PRF 64 MHz: valid codes 9, 10, 11, 12 */
#define UWB_PHY_RX_CODE              9u   /* Must equal UWB_PHY_TX_CODE for all network nodes */

/* =========================================================================
 * SFD mode
 *
 * Standard IEEE 802.15.4 SFD (nsSFD = 0, 8 symbols at 6.8 Mbps).
 *
 * Rationale:
 *   At 6.8 Mbps the standard SFD and the Decawave non-standard SFD are
 *   both 8 symbols long, so there is no airtime difference between them
 *   for this configuration.  Standard SFD (nsSFD = 0) is chosen for
 *   interoperability and IEEE 802.15.4a compliance.
 *
 *   Note: at 110 kbps the two SFD modes differ (64 symbols standard vs
 *   64 Decawave-defined with different sequences); the choice matters there
 *   but has no practical effect at 6.8 Mbps where both are 8 symbols.
 * ========================================================================= */
#define UWB_PHY_NS_SFD               0u   /* 0 = standard IEEE SFD; 1 = Decawave non-standard */

/* =========================================================================
 * Data rate (DWT_BR_6M8 = 2)
 *
 * 6.8 Mbps.
 *
 * Rationale and budget reconciliation:
 *   The contracts/uwb/README.md airtime table explicitly states "Data
 *   (17 B at 6.8 Mbps) approx 20 us", directly specifying 6.8 Mbps as the
 *   data rate.  At 6.8 Mbps the data payload of the largest defined frame
 *   (TWR_FINAL at 27 bytes) contributes approx 32 us, still comfortably
 *   within budget alongside PLEN 64.
 *
 *   The prior-generation firmware used 850 kbps (DWT_BR_850K = 1).  At
 *   850 kbps, 17 bytes of payload alone takes approx 160 us, already
 *   exceeding the 150 us total blink budget before preamble or PHR.
 *   850 kbps is explicitly retired; do not revert.
 *
 *   PHR note: the DW1000 always transmits the PHR at 850 kbps regardless
 *   of the configured data rate.  This is a DW1000 hardware characteristic,
 *   not a software configuration choice.  The "PHR (at 850 kbps) approx
 *   22 us" entry in the README airtime table applies to 6.8 Mbps operation.
 * ========================================================================= */
#define UWB_PHY_DATA_RATE            2u   /* DWT_BR_6M8 = 2; DWT_BR_850K = 1; DWT_BR_110K = 0 */

/* =========================================================================
 * PHR mode (DWT_PHRMODE_STD = 0)
 *
 * Standard PHR mode.
 *
 * Rationale:
 *   Standard PHR mode (DWT_PHRMODE_STD = 0) supports frame payloads up to
 *   127 bytes.  All frame types defined in uwb_frames.h are well within
 *   this limit: the largest frame is TWR_FINAL at 27 bytes PSDU.
 *
 *   Extended frame PHR (DWT_PHRMODE_EXT = 3) supports up to 1023 bytes
 *   but is unnecessary here and is less compatible with standard IEEE
 *   802.15.4 test equipment.
 * ========================================================================= */
#define UWB_PHY_PHR_MODE             0u   /* DWT_PHRMODE_STD = 0x0; DWT_PHRMODE_EXT = 0x3 */

/* =========================================================================
 * SFD timeout — RX only (symbols)
 *
 * 65 symbols.
 *
 * Rationale:
 *   DW1000 recommended formula (User Manual, SFD timeout section):
 *
 *     sfdTO = txPreambLength + 1 + SFD_length - PAC_size
 *
 *   For this configuration:
 *     txPreambLength = 64   (PLEN 64, DWT_PLEN_64)
 *     SFD_length     =  8   (standard SFD at 6.8 Mbps, IEEE 802.15.4a)
 *     PAC_size       =  8   (PAC 8, DWT_PAC8)
 *
 *     sfdTO = 64 + 1 + 8 - 8 = 65 symbols
 *
 *   This is the tightest correct value for this preamble/SFD/PAC
 *   combination.  A tight timeout reduces time wasted on corrupted frames.
 *   The DW1000 hardware default (DWT_SFDTOC_DEF = 0x1041 = 4161) is far
 *   too permissive for a 64-symbol preamble and wastes receiver energy.
 * ========================================================================= */
#define UWB_PHY_SFD_TIMEOUT          65u  /* symbols: 64 + 1 + 8 - 8 = 65 */

/* =========================================================================
 * TX power profile — channel 5
 *
 * PG Delay (TC_PGDELAY, register 0x2A:0B):
 *   0xC0 -- the Decawave-specified pulse generator delay constant for
 *   channel 5.  Taken directly from TC_PGDELAY_CH5 in deca_regs.h.
 *   This calibration constant controls the TX bandwidth shape and must
 *   not be changed without RF spectrum analysis.
 *
 * TX Power Register (TX_POWER, 0x1E):
 *   Production firmware and anchor code MUST use the OTP-programmed value:
 *
 *     uint32_t tx_pwr;
 *     dwt_readtxpower(&tx_pwr);    // read factory-calibrated OTP value
 *     dwt_txconfig_t txcfg = { UWB_PHY_TX_PGDELAY, tx_pwr };
 *     dwt_configuretxrf(&txcfg);
 *
 *   OTP values are programmed at manufacture and compensate for per-device
 *   process variation; using them is required for regulatory compliance.
 *
 * UWB_PHY_TX_POWER_NOMINAL (development / simulation use only):
 *   The value 0x0E082848 is the Decawave nominal non-boosted TX power for
 *   channel 5, PRF 64 MHz, smart power DISABLED, from the DW1000 User
 *   Manual.  Use it ONLY when:
 *     (a) OTP has not been programmed (blank development hardware), or
 *     (b) configuring a simulation or test harness (contracts/sim/).
 *
 *   DO NOT use UWB_PHY_TX_POWER_NOMINAL in production hardware without
 *   verifying regulatory compliance for your target market.
 *
 * Smart TX power (dwt_setsmarttxpower):
 *   Smart power is DISABLED in this configuration.  The nominal power value
 *   above applies uniformly across SHR, PHR, and DATA portions of the frame.
 *   Enabling smart power requires per-device characterisation and is left to
 *   the implementation.
 * ========================================================================= */
#define UWB_PHY_TX_PGDELAY           0xC0u          /* TC_PGDELAY_CH5 (Decawave-specified) */
#define UWB_PHY_TX_POWER_NOMINAL     0x0E082848UL   /* ch5, PRF 64 MHz, non-boosted -- dev/sim only */

/* =========================================================================
 * Compile-time consistency checks
 *
 * These assertions verify that the constants are internally consistent.
 * They fire at compile time if values become mutually incompatible after
 * an edit.  Fix the constant; do not relax or remove any assertion.
 * ========================================================================= */

#ifdef __cplusplus
#  define UWB_PHY_STATIC_ASSERT(cond, msg)  static_assert((cond), msg)
#else
#  define UWB_PHY_STATIC_ASSERT(cond, msg)  _Static_assert((cond), msg)
#endif

/* PAC must be 0 (PAC8) for PLEN 64 -- DW1000 hardware requirement */
UWB_PHY_STATIC_ASSERT(UWB_PHY_PAC == 0u,
    "UWB_PHY_PAC must be 0 (PAC8) for PLEN 64; update both if PLEN changes");

/* SFD timeout must satisfy: PLEN_64 + 1 + SFD_8 - PAC_8 = 65 */
UWB_PHY_STATIC_ASSERT(UWB_PHY_SFD_TIMEOUT == 65u,
    "UWB_PHY_SFD_TIMEOUT must be 65 (= 64 + 1 + 8 - 8); update if PLEN/PAC/SFD changes");

/* TX and RX preamble codes must match -- all network nodes use the same code */
UWB_PHY_STATIC_ASSERT(UWB_PHY_TX_CODE == UWB_PHY_RX_CODE,
    "UWB_PHY_TX_CODE and UWB_PHY_RX_CODE must match; a mismatch breaks reception");

/* Channel must be one of the six DW1000-supported values */
UWB_PHY_STATIC_ASSERT(
    UWB_PHY_CHANNEL == 1u || UWB_PHY_CHANNEL == 2u || UWB_PHY_CHANNEL == 3u ||
    UWB_PHY_CHANNEL == 4u || UWB_PHY_CHANNEL == 5u || UWB_PHY_CHANNEL == 7u,
    "UWB_PHY_CHANNEL must be one of {1, 2, 3, 4, 5, 7}");

/* SFD mode must be 0 (standard) or 1 (Decawave non-standard) */
UWB_PHY_STATIC_ASSERT(UWB_PHY_NS_SFD == 0u || UWB_PHY_NS_SFD == 1u,
    "UWB_PHY_NS_SFD must be 0 (standard) or 1 (Decawave non-standard)");

/* Data rate must be one of the three DW1000 values (0, 1, 2) */
UWB_PHY_STATIC_ASSERT(
    UWB_PHY_DATA_RATE == 0u || UWB_PHY_DATA_RATE == 1u || UWB_PHY_DATA_RATE == 2u,
    "UWB_PHY_DATA_RATE must be 0 (110K), 1 (850K), or 2 (6M8)");

/* PHR mode: 0 = standard, 3 = extended -- the only two DW1000 values */
UWB_PHY_STATIC_ASSERT(UWB_PHY_PHR_MODE == 0u || UWB_PHY_PHR_MODE == 3u,
    "UWB_PHY_PHR_MODE must be 0 (standard) or 3 (extended)");

#ifdef __cplusplus
}
#endif

#endif /* UWB_PHY_CONFIG_H */
