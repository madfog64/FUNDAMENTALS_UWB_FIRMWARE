cmake_minimum_required(VERSION 3.20.0)

# Zephyr unit test build for the uwb_cycle_ref tag-side cycle reference
# module (UWB-243).
#
# Key differences from regular Zephyr app builds:
#   - 'project()' comes BEFORE 'find_package()'
#   - 'COMPONENTS unittest' selects the minimal, kernel-free build
#   - The build target is 'testbinary' (not 'app')
#   - No device tree, no real SPI/GPIO, no deca_driver
#
# What is compiled:
#   uwb_cycle_ref.c       — the module under test (UWB-243); pure logic over
#                           a plain struct (no deca_driver, no radio) --
#                           dw1000_sync.h is included only for the
#                           uwb_sync_info_t type (UWB-242), and dw1000_sync.c
#                           itself is NOT compiled here.
#   mock_log.c            — stubs z_log_minimal_printk() (same rationale and
#                           pattern as tests/dw1000_sync/src/mock_log.c): the
#                           unit_testing defconfig enables CONFIG_LOG=y, so
#                           uwb_cycle_ref.c's LOG_WRN() call (cycle_seq
#                           discontinuity) needs this symbol at link time.
#   test_uwb_cycle_ref.c  — ztest assertions
#
# Platform: unit_testing  (native host executable, no emulation)
# Run:  west twister -p unit_testing -T tests/uwb_cycle_ref -v

project(test_uwb_cycle_ref)
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

set(DW1000_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000)

target_sources(testbinary PRIVATE
    src/test_uwb_cycle_ref.c
    src/mock_log.c
    ${DW1000_DIR}/uwb_cycle_ref.c
)

target_include_directories(testbinary PRIVATE
    ${DW1000_DIR}  # uwb_cycle_ref.h, dw1000_sync.h (uwb_sync_info_t only)
)

# CONFIG_UWB_SYNC_MAX_MISSED is injected as a compile-time constant rather
# than via Kconfig, because the unit_testing platform's Kconfig root does not
# include the drivers/dw1000/Kconfig hierarchy (same rationale as
# tests/dw1000_config's CONFIG_DW1000_PHY_* injection). Value matches the
# Kconfig default in drivers/dw1000/Kconfig.
target_compile_definitions(testbinary PRIVATE
    CONFIG_UWB_SYNC_MAX_MISSED=3
)
