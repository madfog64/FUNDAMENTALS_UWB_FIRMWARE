cmake_minimum_required(VERSION 3.20.0)

# Zephyr unit test build for the battery raw-ADC-counts -> millivolts/
# percentage conversion helpers (UWB-322). Mirrors tests/ble_adv_policy's
# pure-decision host-test pattern (UWB-320).
#
# What is compiled:
#   battery.c          — the module under test (UWB-322); pure conversion
#                          arithmetic, no Zephyr includes.
#   test_battery.c      — ztest assertions
#
# battery_zephyr.c (the real SAADC read + timer + os_mgmt wiring, which
# needs the Zephyr ADC driver and, for the os_mgmt hook, the MCUmgr
# subsystem) is intentionally NOT compiled here — that half is
# Zephyr/hardware-only and is exercised by the real board build + the
# README "Hardware bring-up checklist", not this host test.
#
# Platform: unit_testing  (native host executable, no emulation)
# Run:  west twister -p unit_testing -T tests/battery -v

project(test_battery)
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

target_sources(testbinary PRIVATE
    src/test_battery.c
    ${SRC_DIR}/battery.c
)

target_include_directories(testbinary PRIVATE
    ${SRC_DIR}  # battery.h
)
