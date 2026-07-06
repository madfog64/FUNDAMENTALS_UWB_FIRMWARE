cmake_minimum_required(VERSION 3.20.0)

# Zephyr unit test build for the uwb_tracking per-cycle blinker loop
# (UWB-252).
#
# Mirrors tests/twr_responder's unit_testing pattern: a mocked deca_driver so
# assertions can inspect the exact register/API arguments the composed
# dw1000_rx() (via dw1000_sync_rx()) and dw1000_tx_at() calls drive, without
# any real SPI/hardware. Unlike tests/twr_responder, a single
# uwb_tracking_run_cycle() call drives at most ONE dw1000_rx() and ONE
# dw1000_tx_at() (see uwb_tracking.h), so the simple single-shot mock state
# from tests/dw1000_sync/ suffices -- no RX queueing is needed.
#
# What is compiled:
#   uwb_tracking.c         — the module under test (UWB-252)
#   dw1000_sync.c           — tag-side SYNC RX + validate/parse (UWB-242)
#   dw1000_ranging.c        — dw1000_rx() / dw1000_tx_at() / delayed-TX time
#                             helpers (UWB-231), reused (not reimplemented)
#   uwb_cycle_ref.c         — tag-side maintained cycle reference (UWB-243)
#   uwb_slot_timing.c       — TDMA slot-timing computation (UWB-251)
#   uwb_blink_codec.c       — TDoA tag-blink frame build/parse codec (UWB-250)
#   mock_deca_driver.c      — mocks the deca_driver API surface both
#                             dw1000_rx() and dw1000_tx_at() call (copied
#                             verbatim from tests/dw1000_sync/)
#   mock_log.c              — stubs z_log_minimal_printk() (same rationale
#                             and pattern as tests/dw1000_sync/src/mock_log.c)
#   test_tracking.c         — ztest assertions
#
# Platform: unit_testing  (native host executable, no emulation)
# Run:  west twister -p unit_testing -T tests/tracking -v

project(test_tracking)
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

set(DW1000_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000)

target_sources(testbinary PRIVATE
    src/test_tracking.c
    src/mock_deca_driver.c
    src/mock_log.c
    ${DW1000_DIR}/uwb_tracking.c
    ${DW1000_DIR}/dw1000_sync.c
    ${DW1000_DIR}/dw1000_ranging.c
    ${DW1000_DIR}/uwb_cycle_ref.c
    ${DW1000_DIR}/uwb_slot_timing.c
    ${DW1000_DIR}/uwb_blink_codec.c
)

target_include_directories(testbinary PRIVATE
    ${DW1000_DIR}/deca_driver        # deca_device_api.h, deca_regs.h
    ${DW1000_DIR}                    # uwb_tracking.h, dw1000_sync.h, etc.
    ${CMAKE_CURRENT_SOURCE_DIR}/src  # mock_deca_driver.h
)

# CONFIG_UWB_SYNC_MAX_MISSED is injected as a compile-time constant rather
# than via Kconfig, because the unit_testing platform's Kconfig root does not
# include the drivers/dw1000/Kconfig hierarchy (same rationale as
# tests/uwb_cycle_ref's identical injection). Value matches the Kconfig
# default in drivers/dw1000/Kconfig.
target_compile_definitions(testbinary PRIVATE
    CONFIG_UWB_SYNC_MAX_MISSED=3
)
