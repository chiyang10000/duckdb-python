cmake_minimum_required(VERSION 2.8.12...3.29)

project(TwnExtension)

include_directories(include)

set(EXTENSION_SOURCES twn_extension.cpp)
build_static_extension(twn ${EXTENSION_SOURCES})

set(PARAMETERS "-warnings")
build_loadable_extension(twn ${PARAMETERS} ${EXTENSION_SOURCES})

install(
  TARGETS twn_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
