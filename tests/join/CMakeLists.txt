cmake_minimum_required(VERSION 3.20.0)

# Zephyr unit test build for the tag-side Aloha join state machine (UWB-261).
#
# Mirrors tests/tracking's unit_testing pattern: a mocked deca_driver so
# assertions can inspect the exact register/API arguments the composed
# dw1000_sync_rx() (GATE phase), dw1000_rx() (AWAIT phase), and dw1000_tx_at()
# (REQUEST phase) calls drive, without any real SPI/hardware. A single
# uwb_join_step() call drives AT MOST one RX-family call OR one TX call (see
# uwb_join.h), so the simple single-shot mock state from tests/tracking/ /
# tests/dw1000_sync/ suffices -- no RX queueing is needed.
#
# What is compiled:
#   uwb_join.c              — the module under test (UWB-261)
#   uwb_join_codec.c         — Aloha join frame build/parse codec (UWB-259),
#                              reused (not reimplemented)
#   dw1000_sync.c            — tag-side SYNC RX + validate/parse (UWB-242),
#                              reused for the GATE phase
#   dw1000_ranging.c         — dw1000_rx() / dw1000_tx_at() / delayed-TX time
#                              helpers (UWB-231), reused (not reimplemented)
#   mock_deca_driver.c       — mocks the deca_driver API surface dw1000_rx()
#                              and dw1000_tx_at() call (copied verbatim from
#                              tests/tracking/)
#   mock_log.c               — stubs z_log_minimal_printk() (same rationale
#                              and pattern as tests/tracking/src/mock_log.c)
#   test_join.c               — ztest assertions
#
# Note: uwb_join.c has NO Kconfig dependency of its own (every tunable --
# gate/await/backoff cycles, timeouts, tx_margin_dtu -- is caller-injected via
# uwb_join_config_t, not read from CONFIG_* symbols), and it does not compile
# uwb_cycle_ref.c (the GATE phase only needs dw1000_sync_rx(), not the
# maintained cycle reference) -- so no Kconfig injection is needed here,
# unlike tests/tracking's CONFIG_UWB_SYNC_MAX_MISSED injection.
#
# Platform: unit_testing  (native host executable, no emulation)
# Run:  west twister -p unit_testing -T tests/join -v

project(test_join)
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

set(DW1000_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000)

target_sources(testbinary PRIVATE
    src/test_join.c
    src/mock_deca_driver.c
    src/mock_log.c
    ${DW1000_DIR}/uwb_join.c
    ${DW1000_DIR}/uwb_join_codec.c
    ${DW1000_DIR}/dw1000_sync.c
    ${DW1000_DIR}/dw1000_ranging.c
)

target_include_directories(testbinary PRIVATE
    ${DW1000_DIR}/deca_driver        # deca_device_api.h, deca_regs.h
    ${DW1000_DIR}                    # uwb_join.h, uwb_join_codec.h, dw1000_sync.h, etc.
    ${CMAKE_CURRENT_SOURCE_DIR}/src  # mock_deca_driver.h
)
