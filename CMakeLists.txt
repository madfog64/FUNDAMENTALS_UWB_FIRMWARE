cmake_minimum_required(VERSION 3.20.0)

# Board overlay for DWM1001 pin assignments is at boards/nrf52dk_nrf52832.overlay.
# Zephyr picks it up automatically when building with -b nrf52dk_nrf52832.

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(uwb_tag_firmware VERSION 0.1.0)

target_sources(app PRIVATE src/main.c)
