cmake_minimum_required(VERSION 3.20.0)

# The root Kconfig (uwb_tag_firmware/Kconfig) is scoped to APPLICATION_SOURCE_DIR.
# For samples, APPLICATION_SOURCE_DIR is this directory, not the repo root, so we
# provide our own Kconfig (samples/dw1000_devid/Kconfig) that sources the dw1000
# Kconfig with the correct relative path.

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dw1000_devid_sample VERSION 0.1.0)

target_sources(app PRIVATE src/main.c)

# Pull in the vendored DW1000 driver (gated by CONFIG_DW1000 in prj.conf).
# The path is relative to this CMakeLists.txt so it works from any workspace root.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/dw1000 dw1000)
