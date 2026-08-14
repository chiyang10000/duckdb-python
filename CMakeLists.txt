cmake_minimum_required(VERSION 3.29)

project(duckdb_py LANGUAGES CXX)

set(HFT_ENV_ROOT "" CACHE PATH "Path to the external hft-env checkout")
if(NOT HFT_ENV_ROOT)
  message(FATAL_ERROR "HFT_ENV_ROOT is required to build the twn extension")
endif()
set(HFT_LIBRARY_DIR "${HFT_ENV_ROOT}/hft/lib")
if(NOT EXISTS "${HFT_ENV_ROOT}/hft/include/hft.h")
  message(FATAL_ERROR "Missing ${HFT_ENV_ROOT}/hft/include/hft.h")
endif()
if(NOT EXISTS "${HFT_LIBRARY_DIR}/libhft.so")
  message(FATAL_ERROR "Missing ${HFT_LIBRARY_DIR}/libhft.so")
endif()

# Always use C++11
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Set the library name
set(DUCKDB_PYTHON_LIB_NAME "_duckdb")

# Detect CCache
include(cmake/compiler_launcher.cmake)
setup_compiler_launcher_if_available()

# ────────────────────────────────────────────
# IDE support
# ────────────────────────────────────────────
# Create compile_commands.json for IntelliSense and clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ────────────────────────────────────────────
# Policy hygiene
# ────────────────────────────────────────────
if(POLICY CMP0148) # Disallow FindPythonLibs
  cmake_policy(SET CMP0148 NEW)
endif()

if(POLICY CMP0003) # No implicit link directories
  cmake_policy(SET CMP0003 NEW)
endif()

# ────────────────────────────────────────────
# Dependencies
# ────────────────────────────────────────────
# PyBind11
find_package(pybind11 REQUIRED CONFIG)

# DuckDB
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=x86-64-v3")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=x86-64-v3")
include(cmake/duckdb_loader.cmake)
list(APPEND DUCKDB_EXTENSION_CONFIGS
     "${CMAKE_CURRENT_SOURCE_DIR}/extension/twn/extension_config.cmake")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  duckdb_configure_for_debug()
else()
  duckdb_configure_for_release()
endif()
duckdb_add_library(duckdb_target)
if(NOT "twn" IN_LIST BUILD_EXTENSIONS)
  list(APPEND BUILD_EXTENSIONS twn)
endif()

# Bundle in INTERFACE library
add_library(_duckdb_dependencies INTERFACE)
target_link_libraries(_duckdb_dependencies INTERFACE pybind11::pybind11
                                                     duckdb_target)
# Also add include directory
target_include_directories(
  _duckdb_dependencies
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/duckdb_py/include>
)

# We link duckdb_static. Without this define, duckdb.h marks C API symbols
# __declspec(dllimport) on Windows, producing unresolvable __imp_* references at
# link time. No-op on non-Windows.
target_compile_definitions(_duckdb_dependencies INTERFACE DUCKDB_STATIC_BUILD)

# ────────────────────────────────────────────
# Descend into the real DuckDB‑Python sources
# ────────────────────────────────────────────
add_subdirectory(src/duckdb_py)

pybind11_add_module(
  _duckdb
  $<TARGET_OBJECTS:python_src>
  $<TARGET_OBJECTS:python_arrow>
  $<TARGET_OBJECTS:python_common>
  $<TARGET_OBJECTS:python_functional>
  $<TARGET_OBJECTS:python_jupyter>
  $<TARGET_OBJECTS:python_native>
  $<TARGET_OBJECTS:python_numpy>
  $<TARGET_OBJECTS:python_pandas>
  $<TARGET_OBJECTS:python_pybind11>
  $<TARGET_OBJECTS:python_connection>
  $<TARGET_OBJECTS:python_expression>
  $<TARGET_OBJECTS:python_relation>
  $<TARGET_OBJECTS:python_type>)
# add _duckdb_dependencies
target_link_libraries(_duckdb PRIVATE _duckdb_dependencies)
duckdb_link_extensions(_duckdb)
target_link_libraries(_duckdb PRIVATE hft_external)
set_target_properties(
  _duckdb
  PROPERTIES BUILD_RPATH "${HFT_LIBRARY_DIR}"
             INSTALL_RPATH "${HFT_LIBRARY_DIR}")

# ────────────────────────────────────────────
# Put the object file in the correct place
# ────────────────────────────────────────────

# If we're not building through scikit-build-core then we have to set a
# different dest dir
include(GNUInstallDirs)
if(DEFINED SKBUILD_PLATLIB_DIR)
  set(_DUCKDB_PY_INSTALL_DIR "${SKBUILD_PLATLIB_DIR}")
elseif(DEFINED Python_SITEARCH)
  set(_DUCKDB_PY_INSTALL_DIR "${Python_SITEARCH}")
else()
  message(
    WARNING
      "Could not determine Python install dir. Falling back to CMAKE_INSTALL_LIBDIR."
  )
  set(_DUCKDB_PY_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}")
endif()

install(TARGETS _duckdb LIBRARY DESTINATION "${_DUCKDB_PY_INSTALL_DIR}")
