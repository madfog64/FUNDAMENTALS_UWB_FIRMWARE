/*
 * UWB Tag Firmware — main entry point
 *
 * Skeleton: logs a boot banner and idles. All UWB/DW1000 functionality is
 * added in subsequent subissues (UWB-91 deca_driver port, UWB-92 blink app).
 *
 * Build target: nrf52dk_nrf52832 (DWM1001 module, configured via board overlay)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

int main(void)
{
	LOG_INF("UWB Tag Firmware starting — board: %s, built: %s %s",
		CONFIG_BOARD, __DATE__, __TIME__);

	LOG_DBG("Entering idle loop — UWB subsystem not yet initialised");

	while (1) {
		k_sleep(K_MSEC(1000));
	}

	/* Unreachable; main() returning is treated as an error in Zephyr. */
	return 0;
}
