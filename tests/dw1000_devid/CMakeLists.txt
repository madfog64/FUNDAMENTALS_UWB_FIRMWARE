cmake_minimum_required(VERSION 3.20.0)

# Zephyr unit test build — uses the 'unittest' CMake component.
#
# Key differences from regular Zephyr app builds:
#   - 'project()' comes BEFORE 'find_package()'
#   - 'COMPONENTS unittest' selects the minimal, kernel-free build
#   - The build target is 'testbinary' (not 'app')
#   - No device tree, no real SPI/GPIO — deca_port.c is excluded
#
# The test exercises:
#   1. readfromspi header framing: dwt_readdevid() must pass a 1-byte
#      header of 0x00 to readfromspi() (READ | no-subaddr | reg file 0).
#   2. dwt_readdevid() value decode: the driver assembles LE bytes
#      {0x30,0x01,0xCA,0xDE} into 0xDECA0130.
#
# Platform: unit_testing  (native host executable, no emulation).
# Run:  west twister -p unit_testing -T tests/dw1000_devid -v

project(test_dw1000_devid)
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

# Vendored deca_driver sources compiled directly into the test binary.
# deca_device.c includes only standard C headers (<assert.h>, <stdlib.h>)
# and the deca_driver headers — it is safe to compile in a kernel-free env.
# deca_params_init.c provides the const config arrays deca_device.c needs.
# deca_port.c is NOT included — it has Zephyr SPI/GPIO includes.
set(DECA_DRIVER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000/deca_driver)

target_sources(testbinary PRIVATE
    src/test_dw1000_devid.c
    src/mock_platform.c
    ${DECA_DRIVER_DIR}/deca_device.c
    ${DECA_DRIVER_DIR}/deca_params_init.c
)

target_include_directories(testbinary PRIVATE
    ${DECA_DRIVER_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
