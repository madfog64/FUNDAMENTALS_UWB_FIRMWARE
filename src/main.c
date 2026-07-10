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
#include <zephyr/bluetooth/conn.h>
#include <zephyr/dfu/mcuboot.h>

#include <app_version.h>

#include "image_health.h"

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

/* Advertising is (re)started from a work item so it can be re-armed from the
 * disconnect callback (which runs in the BT RX thread, not a context that
 * should block starting a new advertising set directly).
 */
static struct k_work advertise_work;

/* Minimal advertising data: flags + the device name (CONFIG_BT_DEVICE_NAME,
 * "UWB-Tag"). The SMP GATT service itself (advertised implicitly by being
 * connectable + discoverable via GATT service discovery) is registered by
 * CONFIG_MCUMGR_TRANSPORT_BT; it does not need to be listed in the
 * advertising payload for a client to find it after connecting.
 */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void advertise(struct k_work *work)
{
	ARG_UNUSED(work);
	int err;

	bt_le_adv_stop();

	err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("BLE advertising failed to start (err %d)", err);
		return;
	}

	LOG_INF("BLE advertising started as \"%s\"", CONFIG_BT_DEVICE_NAME);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);

	if (err) {
		LOG_ERR("BLE connection failed (err 0x%02x)", err);
		return;
	}

	LOG_INF("BLE central connected — SMP/MCUmgr service available");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);

	LOG_INF("BLE central disconnected (reason 0x%02x) — resuming advertising", reason);
	k_work_submit(&advertise_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

static void bt_ready(int err)
{
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d) — MCUmgr/SMP-over-BLE unavailable", err);
		return;
	}

	LOG_INF("Bluetooth initialised");
	k_work_submit(&advertise_work);
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

	k_work_init(&advertise_work, advertise);

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
