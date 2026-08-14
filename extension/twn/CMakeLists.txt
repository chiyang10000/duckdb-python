cmake_minimum_required(VERSION 2.8.12...3.29)

project(TwnExtension)

include_directories(include)

add_library(hft_external SHARED IMPORTED GLOBAL)
set_target_properties(
  hft_external
  PROPERTIES IMPORTED_LOCATION "${HFT_LIBRARY_DIR}/libhft.so"
             IMPORTED_NO_SONAME TRUE
             INTERFACE_INCLUDE_DIRECTORIES "${HFT_ENV_ROOT}/hft/include")

set(EXTENSION_SOURCES twn_extension.cpp)
build_static_extension(twn ${EXTENSION_SOURCES})

set(PARAMETERS "-warnings")
build_loadable_extension(twn ${PARAMETERS} ${EXTENSION_SOURCES})

foreach(TWN_TARGET twn_extension twn_loadable_extension)
  set_target_properties(
    ${TWN_TARGET}
    PROPERTIES BUILD_RPATH "${HFT_LIBRARY_DIR}"
               INSTALL_RPATH "${HFT_LIBRARY_DIR}")
  target_link_libraries(${TWN_TARGET} hft_external)
endforeach()

install(
  TARGETS twn_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
