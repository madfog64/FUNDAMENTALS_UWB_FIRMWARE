# drivers/dw1000/CMakeLists.txt
#
# Wires the vendored Decawave deca_driver into the Zephyr application build.
# Gated by CONFIG_DW1000 (defined in drivers/dw1000/Kconfig).
#
# Source layout:
#   deca_driver/   Vendored Decawave register/device API — unmodified.
#   port/          Zephyr port shim.
#     deca_port.h       Port boundary header; declares port API for app code.
#     deca_port.c       Real Zephyr SPI + GPIO port (UWB-93 — strong defs).
#     deca_port_stub.c  __weak stub fallbacks (UWB-92 — overridden at link time
#                       by the strong defs in deca_port.c).
#
# Link-time note: deca_port.c provides strong definitions for writetospi,
# readfromspi, decamutexon, decamutexoff, and deca_sleep.  deca_port_stub.c
# provides __weak definitions for the same symbols.  The linker discards the
# weak definitions when it resolves the strong ones — confirmed via the absence
# of "multiple definition" errors and the presence of "zephyr.elf" in the
# build artefacts.

if(CONFIG_DW1000)

  zephyr_library()

  # deca_port.c includes <zephyr/drivers/spi.h> which transitively pulls in
  # <offsets.h> via the chain:
  #   spi.h -> rtio.h -> app_memdomain.h -> linker-defs.h -> offsets.h
  #
  # offsets.h is generated during the build (target: offsets_h).  Without an
  # explicit dependency, Ninja may start compiling deca_port.c in parallel
  # with the offsets generation step, causing a missing-header error on
  # pristine builds.  This add_dependencies call serialises the compile after
  # offsets_h completes.
  add_dependencies(${ZEPHYR_CURRENT_LIBRARY} offsets_h)

  zephyr_library_sources(
    deca_driver/deca_device.c
    deca_driver/deca_params_init.c
    port/deca_port_stub.c      # __weak stubs — silently overridden by deca_port.c
    port/deca_port.c           # strong Zephyr SPI + GPIO port (UWB-93)
    dw1000_config.c            # PHY configuration layer (UWB-155)
    dw1000_ranging.c           # RX-with-timestamp + scheduled TX (UWB-231)
    dw1000_sync.c              # Tag-side SYNC RX + validate/parse (UWB-242)
    uwb_cycle_ref.c            # Tag-side cycle reference (UWB-243)
    uwb_twr_codec.c            # DS-TWR frame build/parse codec (UWB-230)
  )

  # Include paths for compilation units inside this library.
  zephyr_library_include_directories(
    deca_driver
    port
    ${CMAKE_CURRENT_SOURCE_DIR}   # for phy_config.h and dw1000_config.h
  )

  # Export the vendored and port headers globally so application code can
  # do: #include "deca_device_api.h" or #include "deca_port.h" or
  #     #include "dw1000_config.h" or #include "phy_config.h"
  zephyr_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/deca_driver
    ${CMAKE_CURRENT_SOURCE_DIR}/port
    ${CMAKE_CURRENT_SOURCE_DIR}   # dw1000_config.h, phy_config.h
  )

endif()
