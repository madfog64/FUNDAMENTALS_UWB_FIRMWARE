cmake_minimum_required(VERSION 3.20.0)

# *** NOTE — Why we do NOT use add_subdirectory() here ***
#
# When a sample CMakeLists.txt calls add_subdirectory(drivers/dw1000), Zephyr
# is already in "application mode" (the zephyr_prebuilt target exists after
# find_package).  In application mode zephyr_library() inside the subdirectory
# does NOT add the library to the ZEPHYR_LIBS link set — the symbols are
# compiled but never linked.  This is the Zephyr-documented limitation (see
# zephyr_append_cmake_library warning message in extensions.cmake).
#
# The correct pattern for samples that live outside the NCS module tree:
# compile the driver sources directly into the 'app' target.

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dw1000_config_sample VERSION 0.1.0)

set(DW1000_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000)

target_sources(app PRIVATE
    src/main.c
    ${DW1000_DIR}/deca_driver/deca_device.c
    ${DW1000_DIR}/deca_driver/deca_params_init.c
    ${DW1000_DIR}/port/deca_port.c   # strong SPI/GPIO port defs (no stub needed)
    ${DW1000_DIR}/dw1000_config.c
)

target_include_directories(app PRIVATE
    ${DW1000_DIR}/deca_driver   # deca_device_api.h, deca_regs.h
    ${DW1000_DIR}/port          # deca_port.h
    ${DW1000_DIR}               # dw1000_config.h, phy_config.h
)

# deca_port.c includes <zephyr/drivers/spi.h> which pulls in offsets.h
# (generated at build time).  Same dependency as in drivers/dw1000/CMakeLists.
add_dependencies(app offsets_h)
