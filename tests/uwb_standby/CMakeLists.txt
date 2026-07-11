cmake_minimum_required(VERSION 3.20.0)

# Zephyr unit test build for the STANDBY power-state machine (UWB-319).
#
# Key differences from regular Zephyr app builds:
#   - 'project()' comes BEFORE 'find_package()'
#   - 'COMPONENTS unittest' selects the minimal, kernel-free build
#   - The build target is 'testbinary' (not 'app')
#   - No device tree, no real SPI/GPIO, no deca_driver
#
# What is compiled:
#   uwb_standby.c         — the module under test (UWB-319)
#   dw1000_sleep.c        — the UWB-316 sleep/wake seam this module drives
#   dw1000_config.c       — dw1000_wake()'s reconfigure-on-wake dependency
#                            (UWB-155/UWB-156), exercised for pass-through
#                            only (own detailed assertions live in
#                            tests/dw1000_config/ and tests/dw1000_sleep/)
#   dw1000_sync.c          — tag-side SYNC RX + validate/parse (UWB-242),
#                            reused (not reimplemented) for the ASLEEP-phase
#                            presence-check listen
#   dw1000_ranging.c        — dw1000_rx() (UWB-231), reused for the same
#                            presence-check listen
#   mock_deca_driver.c     — union of tests/dw1000_sleep's and tests/join's
#                            deca_driver mocks (see mock_deca_driver.h)
#   mock_port.c            — mocks reset_DW1000(), port_set_dw1000_slowrate/
#                            fastrate(), port_wakeup_dw1000() (copied from
#                            tests/dw1000_sleep/)
#   mock_seq.c             — shared call-order sequence counter (copied from
#                            tests/dw1000_sleep/)
#   test_uwb_standby.c      — ztest assertions
#
# Note: deca_device.c and deca_params_init.c are intentionally NOT compiled
# here, same as tests/dw1000_sleep/ / tests/join/ — all deca_driver API
# functions are replaced with mocks.
#
# CONFIG_DW1000_PHY_* / CONFIG_UWB_SYNC_MAX_MISSED /
# CONFIG_UWB_STANDBY_WAKE_CADENCE_MS are injected as target_compile_definitions
# below rather than via Kconfig, because the unit_testing platform's Kconfig
# root does not include the drivers/dw1000/Kconfig hierarchy (same rationale
# as tests/dw1000_config/, tests/dw1000_sleep/, and tests/uwb_cycle_ref/
# CMakeLists.txt). Values match the Kconfig defaults in drivers/dw1000/Kconfig.
#
# Platform: unit_testing  (native host executable, no emulation)
# Run:  west twister -p unit_testing -T tests/uwb_standby -v

project(test_uwb_standby)
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

set(DW1000_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000)

target_sources(testbinary PRIVATE
    src/test_uwb_standby.c
    src/mock_deca_driver.c
    src/mock_port.c
    src/mock_seq.c
    ${DW1000_DIR}/uwb_standby.c
    ${DW1000_DIR}/dw1000_sleep.c
    ${DW1000_DIR}/dw1000_config.c
    ${DW1000_DIR}/dw1000_sync.c
    ${DW1000_DIR}/dw1000_ranging.c
)

target_include_directories(testbinary PRIVATE
    ${DW1000_DIR}/deca_driver     # deca_device_api.h, deca_regs.h
    ${DW1000_DIR}                 # uwb_standby.h, dw1000_sleep.h, dw1000_sync.h, etc.
    ${CMAKE_CURRENT_SOURCE_DIR}/src  # mock_deca_driver.h, mock_port.h, mock_seq.h
)

# Inject the CONFIG_DW1000_PHY_* compile-time constants that dw1000_config.c
# (dw1000_wake()'s reconfigure dependency) reads. Values match
# contracts/uwb/phy_config.h v0.6 / Kconfig defaults in drivers/dw1000/Kconfig
# — identical to tests/dw1000_sleep/CMakeLists.txt.
target_compile_definitions(testbinary PRIVATE
    CONFIG_DW1000=1
    CONFIG_DW1000_PHY_CHANNEL=5
    CONFIG_DW1000_PHY_PRF=2
    CONFIG_DW1000_PHY_TX_PLEN=0x04
    CONFIG_DW1000_PHY_PAC=0
    CONFIG_DW1000_PHY_TX_CODE=9
    CONFIG_DW1000_PHY_RX_CODE=9
    CONFIG_DW1000_PHY_NS_SFD=0
    CONFIG_DW1000_PHY_DATA_RATE=2
    CONFIG_DW1000_PHY_PHR_MODE=0
    CONFIG_DW1000_PHY_SFD_TIMEOUT=65
    CONFIG_DW1000_PHY_TX_PGDELAY=0xC0
    CONFIG_DW1000_PHY_TX_POWER=0x0E082848
    CONFIG_DW1000_ANTENNA_DELAY_TX=16436
    CONFIG_DW1000_ANTENNA_DELAY_RX=16436

    # uwb_standby.c's own Kconfig-provisioned constants (see
    # drivers/dw1000/Kconfig) — CONFIG_UWB_SYNC_MAX_MISSED is shared/reused
    # from uwb_cycle_ref.c (UWB-243); CONFIG_UWB_STANDBY_WAKE_CADENCE_MS is
    # new (UWB-319). Values match the Kconfig defaults.
    CONFIG_UWB_SYNC_MAX_MISSED=3
    CONFIG_UWB_STANDBY_WAKE_CADENCE_MS=10000
)
