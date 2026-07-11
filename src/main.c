/*
 * UWB Tag Firmware — main entry point
 *
 * Skeleton: logs a boot banner (including the image version, UWB-266), runs
 * the boot-time self-check gate and confirms the image to MCUboot on
 * success (UWB-266, ADR-0040), brings up BLE + the MCUmgr SMP server
 * (UWB-265, ADR-0040), and idles. All UWB/DW1000 functionality beyond the
 * self-check's presence/wiring probe is added in subsequent subissues
 * (UWB-91 deca_driver port, UWB-92 blink app).
 *
 * BLE/MCUmgr scope is deliberately minimal: initialise the BLE peripheral and
 * start connectable advertising so an SMP client (mcumgr CLI / nRF Connect
 * Device Manager) can find and connect to the tag and drive
 * img_mgmt/os_mgmt commands (the handlers themselves are registered
 * automatically by CONFIG_MCUMGR_GRP_IMG / CONFIG_MCUMGR_GRP_OS, no
 * application code needed).
 *
 * BLE advertising duty-cycle policy (UWB-320, ADR-0046 point 4): the actual
 * advertise/stop wiring now lives in ble_adv_policy.{h,c}/
 * ble_adv_policy_zephyr.c. main.c only initialises the policy and drives it
 * with an initial power state -- see bt_ready() below for what state and
 * why. The bench profile (!CONFIG_UWB_TAG_LOW_POWER) always advertises
 * regardless of the requested state, so this is a no-op behaviour change
 * for that profile.
 *
 * Image self-confirm / test-and-rollback / version-set (UWB-266):
 *   See image_health.h for the gate itself and the honest writeup of what
 *   "test-and-rollback" actually means under this repo's MCUboot
 *   overwrite-only swap mode (short version: MCUboot itself cannot revert a
 *   bad-but-bootable image in this configuration — see README.md "MCUboot &
 *   OTA"). The gate below is deliberately conservative: on failure it logs
 *   and leaves the image unconfirmed, but does NOT force a reboot (there is
 *   no previous image to fall back to, so a reboot loop would only make
 *   things worse and could brick the ability to re-flash over BLE/SMP).
 *
 * Build target: nrf52dk_nrf52832 (DWM1001 module, configured via board overlay)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/dfu/mcuboot.h>

#include <app_version.h>

#include "ble_adv_policy.h"
#include "image_health.h"

#if defined(CONFIG_UWB_BATTERY_MONITOR)
#include "battery.h"
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/*
 * Runs the UWB-266 boot self-check gate and, on success, marks the running
 * image permanent via boot_write_img_confirmed(). Idempotent: does nothing
 * if the image is already confirmed (e.g. a normal warm reset of an
 * already-confirmed image, not a fresh update).
 *
 * Called early in main(), before BLE/SMP bring-up, so a broken image is
 * caught (and stays unconfirmed) as soon as possible after boot — see
 * image_health.h for why this repo's overwrite-only MCUboot mode means
 * "unconfirmed" is observability (mcumgr `image list` -> confirmed: false),
 * not an automatic revert trigger.
 */
static void confirm_image_if_healthy(void)
{
	if (boot_is_img_confirmed()) {
		LOG_DBG("Image already confirmed — skipping self-check");
		return;
	}

	if (image_health_run_checks() != IMAGE_HEALTH_OK) {
		LOG_ERR("Boot self-check FAILED — image left unconfirmed");
		return;
	}

	int rc = boot_write_img_confirmed();

	if (rc != 0) {
		LOG_ERR("boot_write_img_confirmed() failed (err %d)", rc);
		return;
	}

	LOG_INF("Boot self-check passed — image confirmed permanent");
}

static void bt_ready(int err)
{
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d) — MCUmgr/SMP-over-BLE unavailable", err);
		return;
	}

	LOG_INF("Bluetooth initialised");

	ble_adv_policy_init();

	/*
	 * Initial power state (UWB-320, ADR-0046 point 4): the STANDBY
	 * power-state machine that would drive ble_adv_policy_set_state()
	 * from real session/SYNC-loss state is a sibling ticket (UWB-319,
	 * not yet implemented) -- see ble_adv_policy.h's "Scope note". Until
	 * then, default to STANDBY at boot: the tag has no active session
	 * yet (join/track orchestration is itself deferred, see the file
	 * header above), so STANDBY -- "advertise, this is the maintenance
	 * window" (ADR-0040) -- is the correct state to boot into.
	 *
	 * Bench profile (!CONFIG_UWB_TAG_LOW_POWER): this call always
	 * enables advertising regardless of the state passed in, so boot
	 * behaviour there is unchanged from before UWB-320.
	 */
	ble_adv_policy_set_state(BLE_ADV_POLICY_STATE_STANDBY);
}

int main(void)
{
	int err;

	LOG_INF("UWB Tag Firmware starting — board: %s, version: %s, built: %s %s",
		CONFIG_BOARD, APP_VERSION_STRING, __DATE__, __TIME__);

	/*
	 * Run the self-check gate as early as practical (before BLE/SMP
	 * bring-up) so a broken image is caught, and stays unconfirmed, as
	 * soon as possible after boot (UWB-266).
	 */
	confirm_image_if_healthy();

#if defined(CONFIG_UWB_BATTERY_MONITOR)
	/*
	 * Battery voltage monitor (UWB-322, ADR-0046 point 5): starts the
	 * low-rate SAADC VDD sample loop (CONFIG_UWB_BATTERY_SAMPLE_INTERVAL_S)
	 * and, when CONFIG_MCUMGR_GRP_OS_INFO_CUSTOM_HOOKS is enabled,
	 * registers the os_mgmt "info" custom hook that surfaces the latest
	 * reading (see battery_zephyr.c). Started before BLE/SMP bring-up so
	 * an os_mgmt query shortly after boot is at least attempting a
	 * reading (the very first sample may still be pending, in which case
	 * battery_monitor_get_latest() reports "no reading yet" -- see
	 * battery.h).
	 */
	int battery_err = battery_monitor_init();

	if (battery_err != 0) {
		LOG_ERR("Battery monitor init failed (err %d) -- continuing without it",
			battery_err);
	}
#endif

	err = bt_enable(bt_ready);
	if (err) {
		LOG_ERR("bt_enable() failed (err %d)", err);
	}

	LOG_DBG("Entering idle loop — UWB subsystem not yet initialised");

	while (1) {
		k_sleep(K_MSEC(1000));
	}

	/* Unreachable; main() returning is treated as an error in Zephyr. */
	return 0;
}
