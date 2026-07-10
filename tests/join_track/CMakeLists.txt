cmake_minimum_required(VERSION 3.20.0)

# Zephyr unit test build for the join -> tracking orchestration seam
# (UWB-263).
#
# Mirrors tests/join's and tests/tracking's unit_testing pattern: a mocked
# deca_driver so assertions can inspect the exact register/API arguments the
# composed dw1000_sync_rx() / dw1000_rx() / dw1000_tx_at() calls drive,
# without any real SPI/hardware. A single uwb_join_track_step() call forwards
# to exactly one of uwb_join_step() or uwb_tracking_run_cycle(), each of
# which itself drives at most one RX-family call and one TX call -- so the
# simple single-shot mock state from tests/join/ / tests/tracking/ suffices,
# no RX queueing is needed.
#
# What is compiled:
#   uwb_join_track.c        — the module under test (UWB-263)
#   uwb_join.c               — tag-side Aloha join state machine (UWB-261),
#                              reused (not reimplemented)
#   uwb_join_codec.c         — Aloha join frame build/parse codec (UWB-259)
#   uwb_tracking.c           — tag-side per-cycle TDoA tracking loop (UWB-252),
#                              reused (not reimplemented)
#   dw1000_sync.c            — tag-side SYNC RX + validate/parse (UWB-242)
#   dw1000_ranging.c         — dw1000_rx() / dw1000_tx_at() / delayed-TX time
#                              helpers (UWB-231)
#   uwb_cycle_ref.c          — tag-side maintained cycle reference (UWB-243)
#   uwb_slot_timing.c        — TDMA slot-timing computation (UWB-251)
#   uwb_blink_codec.c        — TDoA tag-blink frame build/parse codec (UWB-250)
#   mock_deca_driver.c       — mocks the deca_driver API surface (copied
#                              verbatim from tests/tracking/ / tests/join/)
#   mock_log.c               — stubs z_log_minimal_printk()
#   test_join_track.c        — ztest assertions
#
# Platform: unit_testing  (native host executable, no emulation)
# Run:  west twister -p unit_testing -T tests/join_track -v

project(test_join_track)
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

set(DW1000_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000)

target_sources(testbinary PRIVATE
    src/test_join_track.c
    src/mock_deca_driver.c
    src/mock_log.c
    ${DW1000_DIR}/uwb_join_track.c
    ${DW1000_DIR}/uwb_join.c
    ${DW1000_DIR}/uwb_join_codec.c
    ${DW1000_DIR}/uwb_tracking.c
    ${DW1000_DIR}/dw1000_sync.c
    ${DW1000_DIR}/dw1000_ranging.c
    ${DW1000_DIR}/uwb_cycle_ref.c
    ${DW1000_DIR}/uwb_slot_timing.c
    ${DW1000_DIR}/uwb_blink_codec.c
)

target_include_directories(testbinary PRIVATE
    ${DW1000_DIR}/deca_driver        # deca_device_api.h, deca_regs.h
    ${DW1000_DIR}                    # uwb_join_track.h, uwb_join.h, uwb_tracking.h, etc.
    ${CMAKE_CURRENT_SOURCE_DIR}/src  # mock_deca_driver.h
)

# CONFIG_UWB_SYNC_MAX_MISSED is injected as a compile-time constant rather
# than via Kconfig, because the unit_testing platform's Kconfig root does not
# include the drivers/dw1000/Kconfig hierarchy (same rationale as
# tests/tracking's identical injection -- uwb_cycle_ref.c reads it). Value
# matches the Kconfig default in drivers/dw1000/Kconfig.
target_compile_definitions(testbinary PRIVATE
    CONFIG_UWB_SYNC_MAX_MISSED=3
)
