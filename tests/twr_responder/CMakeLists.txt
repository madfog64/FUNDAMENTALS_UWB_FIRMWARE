cmake_minimum_required(VERSION 3.20.0)

# Zephyr unit test build for the twr_responder module (UWB-232).
#
# Mirrors tests/dw1000_sync's unit_testing pattern: the module under test is
# compiled together with its real dependencies (dw1000_ranging.c, UWB-231;
# uwb_twr_codec.c, UWB-230) against a mocked deca_driver, so assertions can
# inspect exactly which frames were built/scheduled without any real
# SPI/hardware.
#
# What is compiled:
#   twr_responder.c         — the module under test (UWB-232)
#   dw1000_ranging.c         — dw1000_rx() / dw1000_tx_at() /
#                              dw1000_delayed_tx_time() (UWB-231), reused
#                              (not reimplemented) by twr_responder.c
#   uwb_twr_codec.c          — DS-TWR frame build/parse (UWB-230), reused by
#                              twr_responder.c
#   mock_deca_driver.c       — mocks the deca_driver API surface
#                              dw1000_ranging.c calls, extended with an RX
#                              queue (see src/mock_deca_driver.h) since a
#                              single twr_responder_run_once() attempt can
#                              call dw1000_rx() up to twice.
#   mock_log.c               — stubs z_log_minimal_printk() (same rationale
#                              as tests/dw1000_ranging/src/mock_log.c): the
#                              unit_testing defconfig enables CONFIG_LOG=y,
#                              so LOG_*() calls in dw1000_ranging.c and
#                              twr_responder.c need this symbol at link time.
#   test_twr_responder.c     — ztest assertions
#
# Note: deca_device.c is intentionally NOT compiled here (same rationale as
# tests/dw1000_ranging / tests/dw1000_sync): the mock replaces every
# deca_driver API function dw1000_ranging.c calls.
#
# Platform: unit_testing  (native host executable, no emulation)
# Run:  west twister -p unit_testing -T tests/twr_responder -v

project(test_twr_responder)
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

set(DW1000_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000)

target_sources(testbinary PRIVATE
    src/test_twr_responder.c
    src/mock_deca_driver.c
    src/mock_log.c
    ${DW1000_DIR}/twr_responder.c
    ${DW1000_DIR}/dw1000_ranging.c
    ${DW1000_DIR}/uwb_twr_codec.c
)

target_include_directories(testbinary PRIVATE
    ${DW1000_DIR}/deca_driver        # deca_device_api.h, deca_regs.h
    ${DW1000_DIR}                    # twr_responder.h, dw1000_ranging.h, uwb_twr_codec.h, uwb_frames.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src  # mock_deca_driver.h
)

# Inject the CONFIG_UWB_TWR_* compile-time constants twr_responder.c reads
# (via Kconfig in a normal Zephyr build; directly here in unit tests -- the
# unit_testing platform's Kconfig root does not include the
# drivers/dw1000/Kconfig hierarchy, same rationale as
# tests/dw1000_config/CMakeLists.txt).
#
# CONFIG_UWB_TWR_RESP_REPLY_DELAY_DTU matches the Kconfig default (~2 ms).
# CONFIG_UWB_TWR_RESPONDER_DEBUG_RANGE=1 enables the debug-range computation
# so test_twr_responder.c can assert uwb_twr_exchange_t::range_mm against a
# hand-calc fixture.
target_compile_definitions(testbinary PRIVATE
    CONFIG_UWB_TWR_RESP_REPLY_DELAY_DTU=127795200
    CONFIG_UWB_TWR_RESPONDER_POLL_TIMEOUT_US=100000
    CONFIG_UWB_TWR_RESPONDER_FINAL_TIMEOUT_US=10000
    CONFIG_UWB_TWR_RESPONDER_DEBUG_RANGE=1
)
