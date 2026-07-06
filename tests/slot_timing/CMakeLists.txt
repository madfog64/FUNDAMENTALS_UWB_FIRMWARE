cmake_minimum_required(VERSION 3.20.0)

# Zephyr unit test build for the uwb_slot_timing module (UWB-251).
#
# Mirrors tests/dw1000_ranging's unit_testing pattern: uwb_slot_timing.c is
# pure arithmetic, but its only external call is dw1000_delayed_tx_time()
# (UWB-231, dw1000_ranging.c) -- linking the real dw1000_ranging.c (instead of
# re-deriving the truncation independently in the test) exercises the actual
# low-9-bit truncation path the acceptance criteria call for. dw1000_ranging.c
# also compiles dw1000_rx()/dw1000_tx_at(), which call into deca_driver, so
# the same mocked deca_driver as tests/dw1000_ranging is linked here purely to
# satisfy those symbols -- this test suite does not exercise them.
#
# What is compiled:
#   uwb_slot_timing.c      — the module under test (UWB-251)
#   dw1000_ranging.c       — real dw1000_delayed_tx_time() (UWB-231)
#   mock_deca_driver.c     — satisfies dw1000_ranging.c's deca_driver calls
#                            (dw1000_rx()/dw1000_tx_at() are compiled but not
#                            exercised by this suite)
#   mock_log.c             — stubs z_log_minimal_printk() (same rationale as
#                            tests/dw1000_ranging/src/mock_log.c)
#   test_slot_timing.c     — ztest assertions
#
# Platform: unit_testing  (native host executable, no emulation)
# Run:  west twister -p unit_testing -T tests/slot_timing -v

project(test_slot_timing)
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

set(DW1000_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000)

target_sources(testbinary PRIVATE
    src/test_slot_timing.c
    src/mock_deca_driver.c
    src/mock_log.c
    ${DW1000_DIR}/uwb_slot_timing.c
    ${DW1000_DIR}/dw1000_ranging.c
)

target_include_directories(testbinary PRIVATE
    ${DW1000_DIR}/deca_driver        # deca_device_api.h, deca_regs.h
    ${DW1000_DIR}                    # uwb_slot_timing.h, dw1000_ranging.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src  # mock_deca_driver.h
)
