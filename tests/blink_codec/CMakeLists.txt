cmake_minimum_required(VERSION 3.20.0)

# Zephyr unit test build for the uwb_blink_codec TDoA tag-blink frame codec
# (UWB-250).
#
# Key differences from regular Zephyr app builds:
#   - 'project()' comes BEFORE 'find_package()'
#   - 'COMPONENTS unittest' selects the minimal, kernel-free build
#   - The build target is 'testbinary' (not 'app')
#   - No device tree, no real SPI/GPIO, no deca_driver
#
# What is compiled:
#   uwb_blink_codec.c  — the module under test (UWB-250); pure byte
#                        manipulation over the uwb_frames.h structs, no
#                        hardware or deca_driver calls, so no mocks are needed.
#   test_blink_codec.c — ztest assertions
#
# Platform: unit_testing  (native host executable, no emulation)
# Run:  west twister -p unit_testing -T tests/blink_codec -v

project(test_blink_codec)
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

set(DW1000_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000)

target_sources(testbinary PRIVATE
    src/test_blink_codec.c
    ${DW1000_DIR}/uwb_blink_codec.c
)

target_include_directories(testbinary PRIVATE
    ${DW1000_DIR}                 # uwb_frames.h, uwb_blink_codec.h
)
