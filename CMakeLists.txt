cmake_minimum_required(VERSION 3.20.0)

# Board overlay for DWM1001 pin assignments is at boards/nrf52dk_nrf52832.overlay.
# Zephyr picks it up automatically when building with -b nrf52dk_nrf52832.

# ---------------------------------------------------------------------------
# Low-power tag profile (ADR-0046, UWB-314)
#
# `-DCONFIG_UWB_TAG_LOW_POWER=y` on the west build command line is a CLI
# Kconfig override. Under this app's sysbuild flow (sysbuild.conf enables
# MCUboot), sysbuild forwards a raw `-DCONFIG_<NAME>=<val>` CLI arg to this
# child image's own cmake invocation as `-DCLI_CONFIG_<NAME>:INTERNAL=<val>`
# (renamed to avoid colliding with Kconfig's own `CONFIG_<NAME>` cache
# variable, which does not exist yet at this point) — verified empirically
# against this repo's pinned NCS v2.7.0 tree (build_lowpower/UWB-314/
# CMakeCache.txt) while implementing this ticket; a plain
# `if(CONFIG_UWB_TAG_LOW_POWER)` check here silently sees nothing, even
# though Kconfig itself (inside find_package(Zephyr) below) correctly
# resolves the same CLI override on its own, independently of this check.
#
# So: check CLI_CONFIG_UWB_TAG_LOW_POWER here, *before* find_package(Zephyr)
# runs Kconfig, to conditionally layer overlay-lowpower.conf on top of
# prj.conf via EXTRA_CONF_FILE (which must be set before find_package(Zephyr)
# — the documented Zephyr mechanism for CMake-time-conditional conf
# fragments). See overlay-lowpower.conf's header comment for what it sets and
# why a .conf fragment (rather than a Kconfig `default`) is required here.
# Default (bench) builds do not pass the CLI override, so neither variable is
# set and EXTRA_CONF_FILE is left untouched — behaviour is unchanged.
# ---------------------------------------------------------------------------
if(CLI_CONFIG_UWB_TAG_LOW_POWER)
  list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/overlay-lowpower.conf")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(uwb_tag_firmware VERSION 0.1.0)

# NOTE: the VERSION above is plain CMake project() metadata, unrelated to the
# MCUboot-signed *image* version. The single source of truth for the image
# version is the top-level VERSION file (UWB-266, ADR-0040) -- Zephyr
# generates include/generated/app_version.h from it (APP_VERSION_STRING
# etc., consumed by src/main.c) and CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION
# defaults from the same file (zephyr/modules/Kconfig.mcuboot), so imgtool
# signs with the identical version reported in the boot log / over SMP.

target_sources(app PRIVATE
  src/main.c
  src/image_health.c          # pure gate decision logic (UWB-266, host-testable)
  src/image_health_zephyr.c   # Zephyr-side check gathering (UWB-266)
  src/ble_adv_policy.c         # pure BLE adv-policy decision (UWB-320, host-testable)
  src/ble_adv_policy_zephyr.c  # Zephyr-side BLE adv start/stop wiring (UWB-320)
)

if(CONFIG_UWB_BATTERY_MONITOR)
  target_sources(app PRIVATE
    src/battery.c          # pure raw-counts -> mV/percent conversion (UWB-322, host-testable)
    src/battery_zephyr.c   # Zephyr-side SAADC read + timer + os_mgmt/log wiring (UWB-322)
  )
endif()

# Vendored Decawave DW1000 driver (enabled via CONFIG_DW1000 in prj.conf).
# The subdirectory CMakeLists.txt gates itself on CONFIG_DW1000.
add_subdirectory(drivers/dw1000)
