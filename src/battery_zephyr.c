/*! ----------------------------------------------------------------------------
 * @file    battery_zephyr.c
 * @brief   Zephyr-side battery voltage monitor wiring (UWB-322, ADR-0046
 *          point 5): SAADC read + low-rate sample timer + os_mgmt/log
 *          surfacing.
 *
 * Not compiled into the tests/battery host-test binary -- this file needs
 * the real Zephyr ADC driver (SAADC) and, when
 * CONFIG_MCUMGR_GRP_OS_INFO_CUSTOM_HOOKS is enabled, the real MCUmgr
 * subsystem, neither of which are available on the 'unit_testing' platform.
 * Exercised by the real board build + the README "Hardware bring-up
 * checklist" (on-hardware bring-up note, UWB-322 acceptance criteria: confirm
 * reported mV tracks a bench supply sweep).
 *
 * What this does:
 *   1. Reads the "zephyr,user" devicetree ADC channel (see
 *      boards/nrf52dk_nrf52832.overlay) against the SAADC's internal 0.6 V
 *      reference at 1/6 gain / 12-bit resolution -- the standard nRF52
 *      "read VDD" configuration (battery.h documents the exact assumptions
 *      battery_raw_to_millivolts() relies on).
 *   2. Re-arms itself on a low-rate delayable work item
 *      (CONFIG_UWB_BATTERY_SAMPLE_INTERVAL_S, seconds-minutes, NOT per
 *      tracking cycle -- ADR-0046 point 5 is explicit that this must not
 *      run at anything like the ~75 Hz tracking rate).
 *   3. Stores the latest (millivolts, percent) reading behind a mutex and
 *      logs it (LOG_INF) every sample.
 *   4. When CONFIG_MCUMGR_GRP_OS_INFO_CUSTOM_HOOKS is enabled, additionally
 *      registers a custom `os_mgmt info` format specifier ('e', "energy")
 *      that appends "battery=<mV>mV,<pct>%" to the info response -- this is
 *      Zephyr's documented app-extension mechanism for os_mgmt's "info"
 *      command (MGMT_EVT_OP_OS_MGMT_INFO_CHECK/APPEND,
 *      zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt.h), i.e. the "custom stat/
 *      echo group entry" ADR-0046 point 5 / this ticket's acceptance
 *      criteria describe -- not a hand-rolled new MCUmgr group. Query with
 *      `mcumgr -c <conn> os info -f e` (or "-f a" for all fields).
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#include <errno.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>

#include "battery.h"

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "battery module (CONFIG_UWB_BATTERY_MONITOR) requires an ADC io-channel in the board \
	devicetree -- see boards/nrf52dk_nrf52832.overlay's 'zephyr,user { io-channels = <&adc 0>; };'"
#endif

/* The battery ADC channel (VDD via NRF_SAADC_VDD, see the board overlay) is
 * the first (and only) io-channel entry on the zephyr,user node. */
static const struct adc_dt_spec battery_adc_chan = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));

static uint16_t adc_raw_sample;
static struct adc_sequence adc_seq = {
	.buffer = &adc_raw_sample,
	/* Buffer size in bytes, not sample count -- one 16-bit raw sample. */
	.buffer_size = sizeof(adc_raw_sample),
};

/* Guards latest_millivolts/latest_percent/latest_valid: written by the
 * sample work handler (system workqueue context), read by
 * battery_monitor_get_latest() (called from the os_mgmt callback, which may
 * run on the MCUmgr transport's own thread -- a different context). */
static struct k_mutex reading_lock;
static uint16_t latest_millivolts;
static uint8_t latest_percent;
static bool latest_valid;

static void battery_sample_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(battery_sample_work, battery_sample_work_handler);

static void battery_sample_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	int err;

	(void)adc_sequence_init_dt(&battery_adc_chan, &adc_seq);

	err = adc_read_dt(&battery_adc_chan, &adc_seq);
	if (err != 0) {
		LOG_WRN("Battery SAADC read failed (err %d) -- keeping last known reading", err);
	} else {
		uint16_t mv = battery_raw_to_millivolts((int32_t)adc_raw_sample);
		uint8_t pct = battery_millivolts_to_percent(mv);

		k_mutex_lock(&reading_lock, K_FOREVER);
		latest_millivolts = mv;
		latest_percent = pct;
		latest_valid = true;
		k_mutex_unlock(&reading_lock);

		LOG_INF("Battery: %u mV (%u%%)", mv, pct);
	}

	/* Re-arm for the next low-rate sample (seconds-minutes, NOT per
	 * tracking cycle -- ADR-0046 point 5). */
	(void)k_work_schedule(&battery_sample_work,
			      K_SECONDS(CONFIG_UWB_BATTERY_SAMPLE_INTERVAL_S));
}

#if defined(CONFIG_MCUMGR_GRP_OS_INFO_CUSTOM_HOOKS)

#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt.h>

/*
 * Custom os_mgmt "info" format specifier for battery status (ADR-0046 point
 * 5, this ticket's "surface over os_mgmt" acceptance criterion).
 *
 * 'e' ("energy") was picked because none of Zephyr's built-in os_mgmt info
 * specifiers use it (s/n/r/v/b/m/p/i/o/a -- see
 * zephyr/subsys/mgmt/mcumgr/grp/os_mgmt/src/os_mgmt.c's format-character
 * switch). Per os_mgmt.h, application-registered format bits must come from
 * OS_MGMT_INFO_FORMAT_USER_CUSTOM_START upward -- this module only needs
 * one bit.
 *
 * Query with (mcumgr CLI): `mcumgr -c <conn> os info -f e` (or "-f a" for
 * all fields, which also includes this one). Response text: appends
 * "battery=<mV>mV,<pct>%" to the info command's "output" string (or
 * "battery=unknown" if no SAADC sample has completed yet -- e.g. right after
 * boot, before the first battery_sample_work_handler() run).
 */
#define BATTERY_OS_MGMT_INFO_FORMAT_CHAR 'e'
#define BATTERY_OS_MGMT_INFO_FORMAT_BIT  OS_MGMT_INFO_FORMAT_USER_CUSTOM_START

static enum mgmt_cb_return battery_os_mgmt_info_cb(uint32_t event,
						    enum mgmt_cb_return prev_status, int32_t *rc,
						    uint16_t *group, bool *abort_more, void *data,
						    size_t data_size)
{
	ARG_UNUSED(prev_status);
	ARG_UNUSED(group);
	ARG_UNUSED(data_size);

	if (event == MGMT_EVT_OP_OS_MGMT_INFO_CHECK) {
		struct os_mgmt_info_check *check_data = (struct os_mgmt_info_check *)data;
		size_t i = 0;

		/* Scan the client-supplied format string for our custom
		 * character, mirroring the built-in format characters'
		 * handling in os_mgmt.c -- each match must increment
		 * *valid_formats so the overall "was every character
		 * recognised" check in os_mgmt_info() passes. */
		while (i < check_data->format->len) {
			if (check_data->format->value[i] == BATTERY_OS_MGMT_INFO_FORMAT_CHAR) {
				*check_data->format_bitmask |= BATTERY_OS_MGMT_INFO_FORMAT_BIT;
				++(*check_data->valid_formats);
			}
			++i;
		}
	} else if (event == MGMT_EVT_OP_OS_MGMT_INFO_APPEND) {
		struct os_mgmt_info_append *append_data = (struct os_mgmt_info_append *)data;

		if (append_data->all_format_specified ||
		    (*append_data->format_bitmask & BATTERY_OS_MGMT_INFO_FORMAT_BIT)) {
			uint16_t mv = 0;
			uint8_t pct = 0;
			bool valid = battery_monitor_get_latest(&mv, &pct);
			int written;

			if (valid) {
				written = snprintf(
					&append_data->output[*append_data->output_length],
					(append_data->buffer_size - *append_data->output_length),
					"%sbattery=%umV,%u%%",
					(*append_data->prior_output ? " " : ""), mv, pct);
			} else {
				written = snprintf(
					&append_data->output[*append_data->output_length],
					(append_data->buffer_size - *append_data->output_length),
					"%sbattery=unknown",
					(*append_data->prior_output ? " " : ""));
			}

			if (written < 0 ||
			    written >= (append_data->buffer_size - *append_data->output_length)) {
				*abort_more = true;
				*rc = -1;
				return MGMT_CB_ERROR_RC;
			}

			*append_data->output_length += (uint16_t)written;
			*append_data->prior_output = true;
			*append_data->format_bitmask &= ~BATTERY_OS_MGMT_INFO_FORMAT_BIT;
		}
	}

	return MGMT_CB_OK;
}

static struct mgmt_callback battery_os_mgmt_check_cb = {
	.callback = battery_os_mgmt_info_cb,
	.event_id = MGMT_EVT_OP_OS_MGMT_INFO_CHECK,
};

static struct mgmt_callback battery_os_mgmt_append_cb = {
	.callback = battery_os_mgmt_info_cb,
	.event_id = MGMT_EVT_OP_OS_MGMT_INFO_APPEND,
};

static void battery_os_mgmt_register(void)
{
	mgmt_callback_register(&battery_os_mgmt_check_cb);
	mgmt_callback_register(&battery_os_mgmt_append_cb);
}

#endif /* CONFIG_MCUMGR_GRP_OS_INFO_CUSTOM_HOOKS */

int battery_monitor_init(void)
{
	int err;

	k_mutex_init(&reading_lock);

	if (!adc_is_ready_dt(&battery_adc_chan)) {
		LOG_ERR("Battery ADC controller %s not ready", battery_adc_chan.dev->name);
		return -ENODEV;
	}

	err = adc_channel_setup_dt(&battery_adc_chan);
	if (err != 0) {
		LOG_ERR("Battery ADC channel setup failed (err %d)", err);
		return err;
	}

#if defined(CONFIG_MCUMGR_GRP_OS_INFO_CUSTOM_HOOKS)
	battery_os_mgmt_register();
#endif

	/* First sample fires immediately; subsequent samples are spaced by
	 * CONFIG_UWB_BATTERY_SAMPLE_INTERVAL_S (see the re-arm in
	 * battery_sample_work_handler()). */
	(void)k_work_schedule(&battery_sample_work, K_NO_WAIT);

	LOG_INF("Battery monitor started (sampling every %d s)",
		CONFIG_UWB_BATTERY_SAMPLE_INTERVAL_S);

	return 0;
}

bool battery_monitor_get_latest(uint16_t *millivolts, uint8_t *percent)
{
	bool valid;

	k_mutex_lock(&reading_lock, K_FOREVER);
	valid = latest_valid;
	if (valid) {
		if (millivolts != NULL) {
			*millivolts = latest_millivolts;
		}
		if (percent != NULL) {
			*percent = latest_percent;
		}
	}
	k_mutex_unlock(&reading_lock);

	return valid;
}
