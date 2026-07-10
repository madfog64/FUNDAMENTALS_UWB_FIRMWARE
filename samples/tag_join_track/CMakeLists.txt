cmake_minimum_required(VERSION 3.20.0)

# *** NOTE — Why we do NOT use add_subdirectory() here ***
#
# See samples/dw1000_config/CMakeLists.txt for the full rationale: samples
# live outside the NCS module tree, so zephyr_library() inside
# drivers/dw1000/CMakeLists.txt would be compiled but never linked if pulled
# in via add_subdirectory(). Instead, compile the driver sources directly
# into the 'app' target, same as every other sample in this repo.

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tag_join_track_sample VERSION 0.1.0)

set(DW1000_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000)

target_sources(app PRIVATE
    src/main.c
    ${DW1000_DIR}/deca_driver/deca_device.c
    ${DW1000_DIR}/deca_driver/deca_params_init.c
    ${DW1000_DIR}/port/deca_port.c   # strong SPI/GPIO port defs (no stub needed)
    ${DW1000_DIR}/dw1000_config.c
    ${DW1000_DIR}/dw1000_ranging.c
    ${DW1000_DIR}/dw1000_sync.c
    ${DW1000_DIR}/uwb_cycle_ref.c
    ${DW1000_DIR}/uwb_slot_timing.c
    ${DW1000_DIR}/uwb_blink_codec.c
    ${DW1000_DIR}/uwb_join_codec.c
    ${DW1000_DIR}/uwb_join.c
    ${DW1000_DIR}/uwb_tracking.c
    ${DW1000_DIR}/uwb_join_track.c
)

target_include_directories(app PRIVATE
    ${DW1000_DIR}/deca_driver   # deca_device_api.h, deca_regs.h
    ${DW1000_DIR}/port          # deca_port.h
    ${DW1000_DIR}               # dw1000_config.h, phy_config.h, dw1000_ranging.h,
                                 # dw1000_sync.h, uwb_cycle_ref.h, uwb_slot_timing.h,
                                 # uwb_blink_codec.h, uwb_join_codec.h, uwb_join.h,
                                 # uwb_tracking.h, uwb_join_track.h, uwb_frames.h
)

# deca_port.c includes <zephyr/drivers/spi.h> which pulls in offsets.h
# (generated at build time). Same dependency as in drivers/dw1000/CMakeLists.
add_dependencies(app offsets_h)
