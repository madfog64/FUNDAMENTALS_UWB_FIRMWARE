cmake_minimum_required(VERSION 3.20.0)

# Board overlay for DWM1001 pin assignments is at boards/nrf52dk_nrf52832.overlay.
# Zephyr picks it up automatically when building with -b nrf52dk_nrf52832.

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
)

# Vendored Decawave DW1000 driver (enabled via CONFIG_DW1000 in prj.conf).
# The subdirectory CMakeLists.txt gates itself on CONFIG_DW1000.
add_subdirectory(drivers/dw1000)
