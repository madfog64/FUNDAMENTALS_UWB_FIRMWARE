# drivers/dw1000/CMakeLists.txt
#
# Wires the vendored Decawave deca_driver into the Zephyr application build.
# Gated by CONFIG_DW1000 (defined in drivers/dw1000/Kconfig).
#
# Source layout:
#   deca_driver/   Vendored Decawave register/device API — unmodified.
#   port/          Zephyr port shim.
#     deca_port.h       Zephyr-specific port header (include by app code).
#     deca_port_stub.c  __weak stub implementations satisfied until UWB-93.

if(CONFIG_DW1000)

  zephyr_library()

  zephyr_library_sources(
    deca_driver/deca_device.c
    deca_driver/deca_params_init.c
    port/deca_port_stub.c
  )

  # Include paths for compilation units inside this library.
  zephyr_library_include_directories(
    deca_driver
    port
  )

  # Export the vendored and port headers globally so application code can
  # do: #include "deca_device_api.h" or #include "deca_port.h"
  zephyr_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/deca_driver
    ${CMAKE_CURRENT_SOURCE_DIR}/port
  )

endif()
