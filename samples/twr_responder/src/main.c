/*! ----------------------------------------------------------------------------
 * @file    main.c
 * @brief   DS-TWR responder mode sample (UWB-232)
 *
 * Runs dw1000_port_init() -> dw1000_configure() -> repeatedly calls
 * twr_responder_run_once() with this device's placeholder short address
 * (CONFIG_TWR_RESPONDER_SAMPLE_SELF_ADDR — real slot/address assignment is
 * the Aloha join/registration flow, UWB-9/10/11, out of scope here), logging
 * the outcome of each attempt.
 *
 * NOTE: This sample requires an on-target run with a real DWM1001 module
 *       exchanging DS-TWR frames with a second board (anchor DS-TWR
 *       initiator, or a throwaway initiator sketch) — see the "On-hardware
 *       bring-up" section in drivers/dw1000/twr_responder.h and this
 *       directory's README.md. For headless CI verification see
 *       tests/twr_responder/ (ztest).
 *
 * Build:   west build -b nrf52dk_nrf52832 samples/twr_responder
 * Flash:   west flash
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "deca_port.h"
#include "dw1000_config.h"
#include "twr_responder.h"

LOG_MODULE_REGISTER(twr_responder_sample, LOG_LEVEL_DBG);

static const char *status_str(twr_responder_status_t status)
{
    switch (status) {
    case TWR_RESPONDER_EXCHANGE_OK:
        return "EXCHANGE_OK";
    case TWR_RESPONDER_NO_POLL:
        return "NO_POLL";
    case TWR_RESPONDER_FOREIGN_FRAME:
        return "FOREIGN_FRAME";
    case TWR_RESPONDER_TX_ERROR:
        return "TX_ERROR";
    case TWR_RESPONDER_NO_FINAL:
        return "NO_FINAL";
    case TWR_RESPONDER_FINAL_MISMATCH:
        return "FINAL_MISMATCH";
    default:
        return "UNKNOWN";
    }
}

int main(void)
{
    int ret;
    const uint16_t self_addr = (uint16_t)CONFIG_TWR_RESPONDER_SAMPLE_SELF_ADDR;

    LOG_INF("DS-TWR responder sample starting -- self_addr = 0x%04X", self_addr);

    ret = dw1000_port_init();
    if (ret < 0) {
        LOG_ERR("dw1000_port_init failed: %d", ret);
        return ret;
    }

    ret = dw1000_configure();
    if (ret < 0) {
        LOG_ERR("dw1000_configure failed: %d -- check SPI wiring", ret);
        return ret;
    }

    LOG_INF("dw1000_configure() OK -- entering DS-TWR responder loop");

    while (1) {
        uwb_twr_exchange_t exchange;
        twr_responder_status_t status = twr_responder_run_once(self_addr, &exchange);

        if (status == TWR_RESPONDER_EXCHANGE_OK) {
            LOG_INF("Exchange 0x%04X with initiator 0x%04X complete: "
                    "T1=%llu T2=%llu T3=%llu T4=%llu T5=%llu T6=%llu",
                    exchange.exchange_id, exchange.initiator_addr,
                    (unsigned long long)exchange.poll_tx_ts,
                    (unsigned long long)exchange.poll_rx_ts,
                    (unsigned long long)exchange.resp_tx_ts,
                    (unsigned long long)exchange.resp_rx_ts,
                    (unsigned long long)exchange.final_tx_ts,
                    (unsigned long long)exchange.final_rx_ts);
            /* exchange.range_mm is only populated when
             * CONFIG_UWB_TWR_RESPONDER_DEBUG_RANGE=y (this sample's default,
             * see prj.conf); twr_responder_run_once() itself already
             * LOG_INF()s it in that case. */
        } else {
            LOG_DBG("twr_responder_run_once(): %s", status_str(status));
        }
    }

    /* Unreachable; main() returning is treated as an error in Zephyr. */
    return 0;
}
