cmake_minimum_required(VERSION 3.20.0)

# Zephyr unit test build for the image_health boot self-check gate decision
# function (UWB-266).
#
# Key differences from regular Zephyr app builds:
#   - 'project()' comes BEFORE 'find_package()'
#   - 'COMPONENTS unittest' selects the minimal, kernel-free build
#   - The build target is 'testbinary' (not 'app')
#   - No device tree, no real SPI/GPIO, no deca_driver, no MCUboot
#
# What is compiled:
#   image_health.c          — the module under test (UWB-266); pure decision
#                              logic over a plain struct, no Zephyr includes.
#   test_image_health.c     — ztest assertions
#
# image_health_zephyr.c (the real check-gathering side, which needs
# devicetree/device_is_ready()/gpio_is_ready_dt()) is intentionally NOT
# compiled here — that half is Zephyr/hardware-only and is exercised by the
# real board build + on-device bring-up, not this host test.
#
# Platform: unit_testing  (native host executable, no emulation)
# Run:  west twister -p unit_testing -T tests/image_health -v

project(test_image_health)
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

target_sources(testbinary PRIVATE
    src/test_image_health.c
    ${SRC_DIR}/image_health.c
)

target_include_directories(testbinary PRIVATE
    ${SRC_DIR}  # image_health.h
)
