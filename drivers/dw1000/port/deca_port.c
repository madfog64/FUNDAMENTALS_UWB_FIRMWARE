/*! ----------------------------------------------------------------------------
 * @file    deca_port.c
 * @brief   Zephyr SPI + GPIO port layer for the Decawave DW1000 UWB driver.
 *
 * Provides strong definitions for the five platform symbols declared in
 * deca_device_api.h:
 *
 *   writetospi      Zephyr SPI scatter-gather write (header + body).
 *   readfromspi     Zephyr SPI full-duplex read (header + dummy TX, body RX).
 *   decamutexon     irq_lock() — disables interrupts on the single-core nRF52832.
 *   decamutexoff    irq_unlock() — restores interrupt state.
 *   deca_sleep      k_msleep().
 *
 * The __weak stubs in deca_port_stub.c are silently discarded by the linker
 * because these definitions are non-weak.
 *
 * Additional port functions (declared in deca_port.h):
 *   dw1000_port_init        one-time setup call (GPIOs, SPI configs, work item)
 *   port_set_dw1000_slowrate  switch to 2 MHz (required before dwt_initialise)
 *   port_set_dw1000_fastrate  switch to 8 MHz (use after dwt_initialise)
 *   reset_DW1000            hardware reset via RESET GPIO
 *   port_enable/disable_dw1000_irq  rising-edge IRQ on/off
 *   Sleep / portGetTickCnt  delay and tick helpers
 *
 * IRQ dispatch model:
 *   The GPIO IRQ fires in ISR context.  Because dwt_isr() calls readfromspi()
 *   (which invokes SPI transactions that may yield), we must NOT call dwt_isr()
 *   directly from the ISR.  Instead the ISR submits dw1000_irq_work to the
 *   system work queue; the work handler calls dwt_isr() from thread context.
 *
 * SPI rate model:
 *   Two spi_dt_spec configs exist: fast (8 MHz, from DT) and slow (2 MHz,
 *   derived at runtime by copying fast and overriding .config.frequency).
 *   g_spi points to the active config; writetospi/readfromspi always use g_spi.
 *
 * Hardware: DWM1001 module (nRF52832 + DW1000), SPI1.
 * Board overlay: boards/nrf52dk_nrf52832.overlay
 * Binding:       zephyr/dts/bindings/ieee802154/decawave,dw1000.yaml (upstream)
 * NCS version:   v2.7.0 (Zephyr 3.6.x)
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include "deca_device_api.h"
#include "deca_port.h"

LOG_MODULE_REGISTER(deca_port, LOG_LEVEL_DBG);

/* ---------------------------------------------------------------------------
 * Devicetree node — declared in boards/nrf52dk_nrf52832.overlay as:
 *   dw1000: dw1000@0 { compatible = "decawave,dw1000"; ... };
 * --------------------------------------------------------------------------- */
#define DW1000_NODE DT_NODELABEL(dw1000)

BUILD_ASSERT(DT_NODE_EXISTS(DW1000_NODE),
    "DT node 'dw1000' not found — check boards/nrf52dk_nrf52832.overlay");

/* ---------------------------------------------------------------------------
 * SPI configuration
 *
 * DW1000 SPI requirements:
 *   - Mode 0 (CPOL=0, CPHA=0), MSB-first, 8-bit words.
 *   - Clock < 3 MHz during dwt_initialise; up to ~20 MHz after.
 *   - We use 2 MHz (slow) and 8 MHz (fast) as the two operating points.
 *
 * dw1000_spi_fast is initialised from the DT property spi-max-frequency = 8 MHz.
 * dw1000_spi_slow is a runtime copy of dw1000_spi_fast with frequency = 2 MHz.
 *
 * g_spi is the currently active config pointer.  It starts NULL; dw1000_port_init
 * sets it to &dw1000_spi_slow so that the first SPI access runs at slow rate.
 * --------------------------------------------------------------------------- */
#define DW1000_SPI_OP \
    (SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB)

#define DW1000_SPI_SLOW_FREQ_HZ 2000000U   /* 2 MHz — below 3 MHz init limit */

/* Fast config: frequency from DT spi-max-frequency (8 MHz). */
static const struct spi_dt_spec dw1000_spi_fast =
    SPI_DT_SPEC_GET(DW1000_NODE, DW1000_SPI_OP, 0);

/* Slow config: copy of fast with frequency overridden; populated in dw1000_port_init. */
static struct spi_dt_spec dw1000_spi_slow;

/* Active config pointer; NULL until dw1000_port_init runs. */
static const struct spi_dt_spec *g_spi;

/*
 * Maximum DW1000 SPI header length.
 *
 * The DW1000 SPI header is 1–3 bytes:
 *   1 byte  = register-file address byte (no sub-index)
 *   2 bytes = address byte + 7-bit sub-index byte
 *   3 bytes = address byte + 8-bit extended sub-index (2 sub-index bytes)
 */
#define DW1000_SPI_MAX_HDR_LEN 3U

/* ---------------------------------------------------------------------------
 * GPIO specs
 * --------------------------------------------------------------------------- */
/* The upstream binding names the IRQ pin "int-gpios" (not "irq-gpios"). */
static const struct gpio_dt_spec dw1000_irq =
    GPIO_DT_SPEC_GET(DW1000_NODE, int_gpios);

static const struct gpio_dt_spec dw1000_reset =
    GPIO_DT_SPEC_GET(DW1000_NODE, reset_gpios);

/* ---------------------------------------------------------------------------
 * IRQ work item (deferred ISR execution)
 * --------------------------------------------------------------------------- */
static struct k_work dw1000_irq_work;
static struct gpio_callback dw1000_irq_cb;

/**
 * @brief Work queue handler — calls dwt_isr() from thread context.
 *
 * dwt_isr() calls readfromspi() which does Zephyr SPI transactions; these
 * are not safe to call from ISR context.  Deferring to the system work queue
 * satisfies this constraint.
 */
static void dw1000_irq_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    dwt_isr();
}

/**
 * @brief GPIO ISR callback — submitted from interrupt context.
 *
 * Must not call dwt_isr() directly.  Submits dw1000_irq_work so the system
 * work queue can call dwt_isr() from a thread.  k_work_submit() is ISR-safe.
 */
static void dw1000_gpio_irq_handler(const struct device *port,
                                     struct gpio_callback *cb,
                                     uint32_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    k_work_submit(&dw1000_irq_work);
}

/* ---------------------------------------------------------------------------
 * Port initialisation
 * --------------------------------------------------------------------------- */
int dw1000_port_init(void)
{
    int ret;

    /* Build slow config: copy all fields from fast, then override frequency. */
    dw1000_spi_slow          = dw1000_spi_fast;
    dw1000_spi_slow.config.frequency = DW1000_SPI_SLOW_FREQ_HZ;

    /* Start with slow rate so the caller can call dwt_initialise() next. */
    g_spi = &dw1000_spi_slow;

    /* Verify the SPI bus device is ready. */
    if (!spi_is_ready_dt(&dw1000_spi_fast)) {
        LOG_ERR("SPI bus not ready");
        return -ENODEV;
    }

    /* Configure RESET GPIO: output, initially de-asserted (pin HIGH for active-low). */
    if (!gpio_is_ready_dt(&dw1000_reset)) {
        LOG_ERR("DW1000 reset GPIO device not ready");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&dw1000_reset, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure DW1000 reset GPIO: %d", ret);
        return ret;
    }

    /* Configure IRQ GPIO: input (interrupt not yet enabled; call port_enable_dw1000_irq
     * after dwt_setinterrupt() has been configured). */
    if (!gpio_is_ready_dt(&dw1000_irq)) {
        LOG_ERR("DW1000 IRQ GPIO device not ready");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&dw1000_irq, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure DW1000 IRQ GPIO: %d", ret);
        return ret;
    }

    /* Initialise the work item used to defer dwt_isr() out of ISR context. */
    k_work_init(&dw1000_irq_work, dw1000_irq_work_handler);

    /* Register the GPIO callback (interrupt enabled separately by port_enable_dw1000_irq). */
    gpio_init_callback(&dw1000_irq_cb, dw1000_gpio_irq_handler,
                       BIT(dw1000_irq.pin));
    ret = gpio_add_callback(dw1000_irq.port, &dw1000_irq_cb);
    if (ret < 0) {
        LOG_ERR("Failed to add DW1000 IRQ callback: %d", ret);
        return ret;
    }

    LOG_DBG("DW1000 port init OK (slow=%u Hz, fast=%u Hz)",
            DW1000_SPI_SLOW_FREQ_HZ, dw1000_spi_fast.config.frequency);
    return 0;
}

/* ---------------------------------------------------------------------------
 * SPI rate switching
 * --------------------------------------------------------------------------- */
void port_set_dw1000_slowrate(void)
{
    g_spi = &dw1000_spi_slow;
    LOG_DBG("DW1000 SPI -> slow (%u Hz)", DW1000_SPI_SLOW_FREQ_HZ);
}

void port_set_dw1000_fastrate(void)
{
    g_spi = &dw1000_spi_fast;
    LOG_DBG("DW1000 SPI -> fast (%u Hz)", dw1000_spi_fast.config.frequency);
}

/* ---------------------------------------------------------------------------
 * IRQ enable / disable
 * --------------------------------------------------------------------------- */
void port_enable_dw1000_irq(void)
{
    gpio_pin_interrupt_configure_dt(&dw1000_irq, GPIO_INT_EDGE_RISING);
}

void port_disable_dw1000_irq(void)
{
    gpio_pin_interrupt_configure_dt(&dw1000_irq, GPIO_INT_DISABLE);
}

/* ---------------------------------------------------------------------------
 * Hardware reset
 *
 * DW1000 RESET is open-drain.  Sequence:
 *   1. Configure RESET as output, drive active (LOW for GPIO_ACTIVE_LOW) → assert reset.
 *   2. Wait 10 ms.
 *   3. Configure RESET as input → release; the DW1000's internal pull-up brings it HIGH.
 *   4. Wait 5 ms for the DW1000 to complete its power-on sequence.
 *
 * The function is safe to call multiple times.
 * --------------------------------------------------------------------------- */
void reset_DW1000(void)
{
    /* Assert reset: output in active state = drives pin LOW (GPIO_ACTIVE_LOW). */
    gpio_pin_configure_dt(&dw1000_reset, GPIO_OUTPUT_ACTIVE);
    k_msleep(10);

    /* Release reset: configure as input so the DW1000's pull-up takes over. */
    gpio_pin_configure_dt(&dw1000_reset, GPIO_INPUT);
    k_msleep(5);

    LOG_DBG("DW1000 hardware reset done");
}

/* ---------------------------------------------------------------------------
 * deca_driver platform symbols — strong definitions
 *
 * These definitions override the __weak stubs in deca_port_stub.c.  The
 * linker discards the weak symbols when it finds these strong definitions in
 * the same archive.
 * --------------------------------------------------------------------------- */

/**
 * @brief SPI write — header + body concatenated in a single CS-assertion.
 *
 * Uses two spi_buf entries (scatter-gather) to avoid copying header and body
 * into a single large stack buffer.  The Zephyr nRF SPIM driver holds CS
 * asserted across all buffers in a single spi_buf_set.
 *
 * The header is copied to a local RAM array because the nRF SPIM EasyDMA
 * requires source buffers to be in SRAM; callers may pass pointers into flash.
 *
 * bodyBuffer must also be in SRAM (EasyDMA constraint); the deca_driver always
 * provides stack or heap buffers, so this is satisfied in practice.
 */
int writetospi(uint16 headerLength, const uint8 *headerBuffer,
               uint32 bodylength, const uint8 *bodyBuffer)
{
    uint8_t hdr[DW1000_SPI_MAX_HDR_LEN];

    if (headerLength > DW1000_SPI_MAX_HDR_LEN || g_spi == NULL) {
        return DWT_ERROR;
    }

    memcpy(hdr, headerBuffer, headerLength);

    const struct spi_buf tx_bufs[2] = {
        { .buf = hdr,              .len = headerLength },
        { .buf = (void *)bodyBuffer, .len = bodylength  },
    };
    const struct spi_buf_set tx_set = {
        .buffers = tx_bufs,
        .count   = 2U,
    };

    return spi_write_dt(g_spi, &tx_set) < 0 ? DWT_ERROR : DWT_SUCCESS;
}

/**
 * @brief SPI read — header out on MOSI, body captured on MISO.
 *
 * Full-duplex scatter-gather:
 *   TX: [header_bytes][ORC_bytes]    — MOSI driven by header, then ORC (0xFF)
 *   RX: [discard_echo][readBuffer]   — header echo discarded, body captured
 *
 * A NULL TX spi_buf with non-zero len causes the nRF SPIM to transmit the
 * ORC byte (default 0xFF) for that length.  The DW1000 ignores MOSI during
 * the read phase, so ORC content is irrelevant.
 *
 * A NULL RX spi_buf with non-zero len causes the nRF SPIM to discard received
 * bytes, used here to throw away the echoed header.
 *
 * readBuffer must be in SRAM (EasyDMA constraint).
 */
int readfromspi(uint16 headerLength, const uint8 *headerBuffer,
                uint32 readlength, uint8 *readBuffer)
{
    uint8_t hdr[DW1000_SPI_MAX_HDR_LEN];

    if (headerLength > DW1000_SPI_MAX_HDR_LEN || g_spi == NULL) {
        return DWT_ERROR;
    }

    memcpy(hdr, headerBuffer, headerLength);

    const struct spi_buf tx_bufs[2] = {
        { .buf = hdr,  .len = headerLength },  /* header out */
        { .buf = NULL, .len = readlength   },  /* ORC on MOSI during read phase */
    };
    const struct spi_buf rx_bufs[2] = {
        { .buf = NULL,       .len = headerLength },  /* discard echoed header */
        { .buf = readBuffer, .len = readlength   },  /* capture register data */
    };
    const struct spi_buf_set tx_set = { .buffers = tx_bufs, .count = 2U };
    const struct spi_buf_set rx_set = { .buffers = rx_bufs, .count = 2U };

    return spi_transceive_dt(g_spi, &tx_set, &rx_set) < 0 ? DWT_ERROR : DWT_SUCCESS;
}

/**
 * @brief Disable interrupts and return the saved state.
 *
 * On the single-core nRF52832, irq_lock() prevents both ISR preemption and
 * cooperative-thread preemption (all thread switches are interrupt-driven).
 * This makes SPI transactions in deca_driver safe from concurrent DW1000 ISR
 * execution, matching the intent of the deca_driver mutex model.
 *
 * The returned key is the irq_lock key cast to decaIrqStatus_t (typedef int).
 */
decaIrqStatus_t decamutexon(void)
{
    return (decaIrqStatus_t)irq_lock();
}

/**
 * @brief Restore interrupt state saved by decamutexon.
 *
 * @param s  IRQ lock key returned by decamutexon.
 */
void decamutexoff(decaIrqStatus_t s)
{
    irq_unlock((unsigned int)s);
}

/**
 * @brief Millisecond sleep — used by deca_driver for timing.
 *
 * @param time_ms  Duration in milliseconds.
 */
void deca_sleep(unsigned int time_ms)
{
    k_msleep((int32_t)time_ms);
}

/* ---------------------------------------------------------------------------
 * Additional port helpers (declared in deca_port.h)
 * --------------------------------------------------------------------------- */

/**
 * @brief Millisecond delay — alias for deca_sleep.
 */
void Sleep(uint32_t time_ms)
{
    k_msleep((int32_t)time_ms);
}

/**
 * @brief Return elapsed time since boot in milliseconds (32-bit, wrapping).
 */
uint32_t portGetTickCnt(void)
{
    return k_uptime_get_32();
}
