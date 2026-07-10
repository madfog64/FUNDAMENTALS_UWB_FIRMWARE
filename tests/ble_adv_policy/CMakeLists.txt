cmake_minimum_required(VERSION 3.20.0)

# Zephyr unit test build for the BLE advertising duty-cycle policy decision
# function (UWB-320). Mirrors tests/image_health's pure-decision host-test
# pattern (UWB-266).
#
# What is compiled:
#   ble_adv_policy.c          — the module under test (UWB-320); pure
#                                decision logic, no Zephyr includes.
#   test_ble_adv_policy.c      — ztest assertions
#
# ble_adv_policy_zephyr.c (the real bt_le_adv_start/stop wiring, which needs
# the Zephyr BT stack) is intentionally NOT compiled here — that half is
# Zephyr/hardware-only and is exercised by the real board build + the
# README "Hardware bring-up checklist", not this host test.
#
# Platform: unit_testing  (native host executable, no emulation)
# Run:  west twister -p unit_testing -T tests/ble_adv_policy -v

project(test_ble_adv_policy)
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

target_sources(testbinary PRIVATE
    src/test_ble_adv_policy.c
    ${SRC_DIR}/ble_adv_policy.c
)

target_include_directories(testbinary PRIVATE
    ${SRC_DIR}  # ble_adv_policy.h
)
