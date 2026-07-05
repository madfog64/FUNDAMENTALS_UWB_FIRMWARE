/*! ----------------------------------------------------------------------------
 * @file    main.c
 * @brief   DW1000 PHY configuration sample (UWB-155)
 *
 * Runs dw1000_configure() then reads back the three key DW1000 registers
 * (CHAN_CTRL, TX_FCTRL, SYS_CFG) and logs their values.  Expected output on
 * a real DWM1001 module reflects the parameters set by dw1000_configure():
 *
 *   CHAN_CTRL (0x1F):  channel + PRF + preamble codes
 *   TX_FCTRL (0x08):  data rate + PRF + preamble length (low 32 bits)
 *   SYS_CFG  (0x04):  PHR mode + RX data rate indicator
 *
 * NOTE: This sample requires an on-target run with a real DWM1001 module.
 *       For headless CI verification see tests/dw1000_config/ (ztest).
 *
 * Build:   west build -b nrf52dk_nrf52832 samples/dw1000_config
 * Flash:   west flash
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "deca_device_api.h"
#include "deca_regs.h"
#include "deca_port.h"
#include "dw1000_config.h"

LOG_MODULE_REGISTER(dw1000_config_sample, LOG_LEVEL_DBG);

int main(void)
{
    int ret;

    LOG_INF("DW1000 PHY configuration sample starting");

    /* Initialise port layer (SPI config, GPIOs, deferred ISR work item). */
    ret = dw1000_port_init();
    if (ret < 0) {
        LOG_ERR("dw1000_port_init failed: %d", ret);
        return ret;
    }

    /*
     * Run the full PHY configuration sequence:
     *   reset -> slow SPI -> dwt_initialise(DWT_LOADUCODE)
     *   -> fast SPI -> dwt_configure -> dwt_configuretxrf
     */
    ret = dw1000_configure();
    if (ret < 0) {
        LOG_ERR("dw1000_configure failed: %d — check SPI wiring", ret);
        return ret;
    }

    LOG_INF("dw1000_configure() OK — reading back key registers");

    /*
     * Read back the three key configuration registers to confirm the PHY
     * parameters were accepted by the DW1000 hardware.
     *
     * CHAN_CTRL (register 0x1F, 4 bytes):
     *   bits  [3:0]  TX channel
     *   bits  [7:4]  RX channel
     *   bits [18:18] DWSFD (non-standard SFD enable)
     *   bits [19:18] RX PRF
     *   bits [26:22] TX preamble code
     *   bits [31:27] RX preamble code
     *
     * TX_FCTRL (register 0x08, 5 bytes; we read low 32 bits):
     *   bits [14:13] TX bit rate
     *   bits [17:16] TX PRF
     *   bits [21:18] TX preamble length (TXPSR + PE)
     *
     * SYS_CFG (register 0x04, 4 bytes):
     *   bits [17:16] PHR mode
     *   bit  [22]    RXM110K (1 = 110 kbps RX mode; 0 = not 110 kbps)
     *
     * For channel 5 / PRF 64 MHz / PLEN 64 / 6.8 Mbps / std PHR:
     *   Expected CHAN_CTRL low byte:  0x55 (TX ch5 | RX ch5)
     *   Expected TX_FCTRL rate bits:  0x4000 (DWT_BR_6M8 << 13)
     *   Expected TX_FCTRL PRF bits:   0x20000 (PRF64M << 16)
     *   Expected TX_FCTRL PLEN bits:  0x40000 (PLEN_64 bits 21:18)
     *   Expected SYS_CFG PHR bits:    0x00000000 (PHR_MODE_STD = 0)
     */
    uint32 chan_ctrl = dwt_read32bitreg(CHAN_CTRL_ID);
    uint32 tx_fctrl = dwt_read32bitreg(TX_FCTRL_ID);
    uint32 sys_cfg  = dwt_read32bitreg(SYS_CFG_ID);

    LOG_INF("CHAN_CTRL (0x1F) = 0x%08X", (unsigned)chan_ctrl);
    LOG_INF("  TX channel:   %u (expected %u)",
            (unsigned)(chan_ctrl & CHAN_CTRL_TX_CHAN_MASK),
            (unsigned)CONFIG_DW1000_PHY_CHANNEL);
    LOG_INF("  RX channel:   %u (expected %u)",
            (unsigned)((chan_ctrl & CHAN_CTRL_RX_CHAN_MASK) >> CHAN_CTRL_RX_CHAN_SHIFT),
            (unsigned)CONFIG_DW1000_PHY_CHANNEL);
    LOG_INF("  RX PRF bits:  0x%05X (expected 0x%05X for PRF64M)",
            (unsigned)(chan_ctrl & CHAN_CTRL_RXFPRF_MASK),
            (unsigned)CHAN_CTRL_RXFPRF_64);
    LOG_INF("  TX pcode:     %u (expected %u)",
            (unsigned)((chan_ctrl & CHAN_CTRL_TX_PCOD_MASK) >> CHAN_CTRL_TX_PCOD_SHIFT),
            (unsigned)CONFIG_DW1000_PHY_TX_CODE);
    LOG_INF("  RX pcode:     %u (expected %u)",
            (unsigned)((chan_ctrl & CHAN_CTRL_RX_PCOD_MASK) >> CHAN_CTRL_RX_PCOD_SHIFT),
            (unsigned)CONFIG_DW1000_PHY_RX_CODE);

    LOG_INF("TX_FCTRL (0x08) low 32-bit = 0x%08X", (unsigned)tx_fctrl);
    LOG_INF("  TX bit rate bits: 0x%05X (expected 0x%05X for 6M8)",
            (unsigned)(tx_fctrl & TX_FCTRL_TXBR_MASK),
            (unsigned)TX_FCTRL_TXBR_6M);
    LOG_INF("  TX PRF bits:      0x%05X (expected 0x%05X for 64M)",
            (unsigned)(tx_fctrl & TX_FCTRL_TXPRF_MASK),
            (unsigned)TX_FCTRL_TXPRF_64M);
    LOG_INF("  TX PLEN bits:     0x%05X (expected 0x%05X for PLEN_64)",
            (unsigned)(tx_fctrl & TX_FCTRL_TXPSR_PE_MASK),
            (unsigned)TX_FCTRL_TXPSR_PE_64);

    LOG_INF("SYS_CFG  (0x04) = 0x%08X", (unsigned)sys_cfg);
    LOG_INF("  PHR mode bits: 0x%05X (expected 0x00000000 for STD)",
            (unsigned)(sys_cfg & SYS_CFG_PHR_MODE_11));
    LOG_INF("  RXM110K bit:   %u (expected 0 for non-110K mode)",
            (unsigned)((sys_cfg & SYS_CFG_RXM110K) ? 1u : 0u));

    LOG_INF("Register readback complete — requires on-device verification");
    LOG_INF("For automated CI checks see tests/dw1000_config/ (unit_testing)");

    /* Idle. */
    while (1) {
        k_sleep(K_MSEC(5000));
        LOG_DBG("Heartbeat — PHY configured, idle");
    }

    return 0;
}
