cmake_minimum_required(VERSION 3.20.0)

# This test targets the 'unit_testing' platform — a native-hosted, kernel-free
# build that compiles ztest suites as a plain Linux executable.  No Zephyr SPI
# or GPIO subsystem is present; the platform functions (readfromspi, writetospi,
# decamutexon, decamutexoff, deca_sleep) are provided by a local mock.
#
# The test exercises two things:
#   1. readfromspi header framing: the deca_driver encodes the DEV_ID register
#      (file id 0x00, index 0) as a single header byte 0x00 (READ | no-subaddr).
#   2. dwt_readdevid() decode: the driver assembles the 4 LE bytes returned by
#      readfromspi into the expected 32-bit value (0xDECA0130).

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_dw1000_devid)

# Vendored deca_driver sources compiled directly into the test.
# Only deca_device.c and deca_params_init.c are needed; deca_port.c is NOT
# included (it has Zephyr SPI/GPIO headers incompatible with unit_testing).
set(DECA_DRIVER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000/deca_driver)

target_sources(app PRIVATE
    src/test_dw1000_devid.c
    src/mock_platform.c
    ${DECA_DRIVER_DIR}/deca_device.c
    ${DECA_DRIVER_DIR}/deca_params_init.c
)

target_include_directories(app PRIVATE
    ${DECA_DRIVER_DIR}
    # Port header exposed for decaIrqStatus_t — but we do NOT compile deca_port.c.
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000/port
)
