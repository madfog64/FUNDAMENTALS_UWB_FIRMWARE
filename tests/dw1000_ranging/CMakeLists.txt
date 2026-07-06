cmake_minimum_required(VERSION 3.20.0)

# Zephyr unit test build for the dw1000_ranging module (UWB-231).
#
# Mirrors tests/dw1000_config's unit_testing pattern: a mocked deca_driver so
# assertions can inspect the exact register/API arguments dw1000_ranging.c
# drives, without any real SPI/hardware.
#
# What is compiled:
#   dw1000_ranging.c        — the module under test (UWB-231)
#   mock_deca_driver.c      — mocks dwt_setrxtimeout(), dwt_rxenable(),
#                             dwt_forcetrxoff(), dwt_read32bitoffsetreg(),
#                             dwt_write32bitoffsetreg(), dwt_readrxdata(),
#                             dwt_readrxtimestamp(), dwt_readtxtimestamp(),
#                             dwt_setdelayedtrxtime(), dwt_writetxdata(),
#                             dwt_writetxfctrl(), dwt_starttx()
#   mock_log.c              — stubs z_log_minimal_printk() (same rationale and
#                             pattern as tests/dw1000_config/src/mock_port.c):
#                             the unit_testing defconfig enables CONFIG_LOG=y,
#                             so dw1000_ranging.c's LOG_ERR()/LOG_DBG() calls
#                             need this symbol at link time.
#   test_dw1000_ranging.c   — ztest assertions
#
# Note: deca_device.c is intentionally NOT compiled here (same rationale as
# tests/dw1000_config): the mock replaces every deca_driver API function
# dw1000_ranging.c calls, so assertions can verify argument values directly
# without SPI decoding.
#
# Platform: unit_testing  (native host executable, no emulation)
# Run:  west twister -p unit_testing -T tests/dw1000_ranging -v

project(test_dw1000_ranging)
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

set(DW1000_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000)

target_sources(testbinary PRIVATE
    src/test_dw1000_ranging.c
    src/mock_deca_driver.c
    src/mock_log.c
    ${DW1000_DIR}/dw1000_ranging.c
)

target_include_directories(testbinary PRIVATE
    ${DW1000_DIR}/deca_driver        # deca_device_api.h, deca_regs.h
    ${DW1000_DIR}                    # dw1000_ranging.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src  # mock_deca_driver.h
)
