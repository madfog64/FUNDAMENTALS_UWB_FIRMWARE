cmake_minimum_required(VERSION 3.20.0)

# Zephyr unit test build for the dw1000_sleep driver sleep/wake seam (UWB-316).
#
# Key differences from regular Zephyr app builds:
#   - 'project()' comes BEFORE 'find_package()'
#   - 'COMPONENTS unittest' selects the minimal, kernel-free build
#   - The build target is 'testbinary' (not 'app')
#   - No device tree, no real SPI/GPIO
#
# What is compiled:
#   dw1000_sleep.c        — the module under test (UWB-316)
#   dw1000_config.c        — dw1000_wake()'s reconfigure-on-wake dependency
#                            (UWB-155/UWB-156) — exercised here for call
#                            order/pass-through only; its own detailed PHY
#                            parameter assertions live in tests/dw1000_config/.
#   mock_deca_driver.c    — mocks dwt_initialise(), dwt_configure(),
#                           dwt_configuretxrf(), dwt_settxantennadelay(),
#                           dwt_setrxantennadelay(), dwt_otpread(),
#                           dwt_configuresleep(), dwt_entersleep()
#   mock_port.c           — mocks reset_DW1000(), port_set_dw1000_slowrate(),
#                           port_set_dw1000_fastrate(), port_wakeup_dw1000()
#   mock_seq.c            — shared call-order sequence counter
#   test_dw1000_sleep.c   — ztest assertions
#
# Note: deca_device.c and deca_params_init.c are intentionally NOT compiled
# here, same as tests/dw1000_config/ — all deca_driver API functions are
# replaced with mocks so assertions can verify argument values and call
# order directly, without SPI decoding.
#
# CONFIG_DW1000_PHY_* values are injected as target_compile_definitions below
# rather than via Kconfig, because the unit_testing platform's Kconfig root
# does not include the drivers/dw1000/Kconfig hierarchy. The values here are
# pinned to phy_config.h v0.6 (same as tests/dw1000_config/CMakeLists.txt and
# the Kconfig defaults).
#
# Platform: unit_testing  (native host executable, no emulation)
# Run:  west twister -p unit_testing -T tests/dw1000_sleep -v

project(test_dw1000_sleep)
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

set(DW1000_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000)

target_sources(testbinary PRIVATE
    src/test_dw1000_sleep.c
    src/mock_deca_driver.c
    src/mock_port.c
    src/mock_seq.c
    ${DW1000_DIR}/dw1000_sleep.c
    ${DW1000_DIR}/dw1000_config.c
)

target_include_directories(testbinary PRIVATE
    ${DW1000_DIR}/deca_driver     # deca_device_api.h, deca_regs.h
    ${DW1000_DIR}                 # dw1000_sleep.h, dw1000_config.h, phy_config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src  # mock_deca_driver.h, mock_port.h, mock_seq.h
)

# Inject the CONFIG_DW1000_PHY_* compile-time constants that dw1000_config.c
# (dw1000_wake()'s reconfigure dependency) reads. Values match
# contracts/uwb/phy_config.h v0.6 / Kconfig defaults in drivers/dw1000/Kconfig
# — identical to tests/dw1000_config/CMakeLists.txt.
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
)
