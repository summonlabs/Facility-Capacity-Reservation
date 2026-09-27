# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Facility Capacity Reservation - DCCP Tranche 2 (Facility Capacity and
# Placement), repository 15 of the canonical 72-runtime DCCP.
#
# The primary product surface is the library target `facility_capacity_reservation`,
# exported as `dccp::facility_capacity_reservation`. The inspection CLI, examples,
# tests and benchmarks are optional; only the CLI is installed.

cmake_minimum_required(VERSION 3.20)

# Keep in sync with include/dccp/facility_capacity_reservation/version.hpp; the
# test suite compares this value against the compiled-in library version.
project(facility_capacity_reservation
        VERSION 1.0.0
        DESCRIPTION "Facility-capacity commitments: reservations, accounting and reconciliation"
        HOMEPAGE_URL "https://github.com/summonlabs/Facility-Capacity-Reservation"
        LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(FACILITY_CAPACITY_RESERVATION_BUILD_TESTS "Build the Facility Capacity Reservation test suite" ON)
option(FACILITY_CAPACITY_RESERVATION_BUILD_TOOLS "Build the fcrctl inspection tool" ON)
option(FACILITY_CAPACITY_RESERVATION_BUILD_EXAMPLES "Build the Facility Capacity Reservation examples" ON)
option(FACILITY_CAPACITY_RESERVATION_BUILD_BENCHMARKS "Build the Facility Capacity Reservation benchmarks" ON)
option(FACILITY_CAPACITY_RESERVATION_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(FACILITY_CAPACITY_RESERVATION_ENABLE_ASAN "Build with AddressSanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Per-configuration optimisation flags
# ---------------------------------------------------------------------------
# The flags are stated explicitly rather than inherited. A surrounding toolchain
# can leave CMAKE_CXX_FLAGS_<CONFIG> empty, and a project that reports a Release
# build must not then produce an unoptimised one. An operator-supplied value is
# left untouched: the defaults are only filled in where nothing was set.

if(MSVC)
  if(NOT CMAKE_CXX_FLAGS_RELEASE)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG" CACHE STRING "Release compiler flags" FORCE)
  endif()
  if(NOT CMAKE_CXX_FLAGS_RELWITHDEBINFO)
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/O2 /Ob2 /Zi /DNDEBUG" CACHE STRING
        "RelWithDebInfo compiler flags" FORCE)
  endif()
  if(NOT CMAKE_CXX_FLAGS_DEBUG)
    set(CMAKE_CXX_FLAGS_DEBUG "/Zi /Ob0 /Od /RTC1 /D_DEBUG" CACHE STRING "Debug compiler flags" FORCE)
  endif()
else()
  if(NOT CMAKE_CXX_FLAGS_RELEASE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Release compiler flags" FORCE)
  endif()
  if(NOT CMAKE_CXX_FLAGS_RELWITHDEBINFO)
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG" CACHE STRING "RelWithDebInfo compiler flags" FORCE)
  endif()
  if(NOT CMAKE_CXX_FLAGS_DEBUG)
    set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g" CACHE STRING "Debug compiler flags" FORCE)
  endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Warnings and sanitizers
# ---------------------------------------------------------------------------

add_library(facility_capacity_reservation_warnings INTERFACE)
add_library(dccp::facility_capacity_reservation_warnings ALIAS facility_capacity_reservation_warnings)

if(MSVC)
  target_compile_options(facility_capacity_reservation_warnings INTERFACE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  if(FACILITY_CAPACITY_RESERVATION_WARNINGS_AS_ERRORS)
    target_compile_options(facility_capacity_reservation_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(facility_capacity_reservation_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align -Wunused
    -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2)
  if(FACILITY_CAPACITY_RESERVATION_WARNINGS_AS_ERRORS)
    target_compile_options(facility_capacity_reservation_warnings INTERFACE -Werror)
  endif()
endif()

add_library(facility_capacity_reservation_sanitizers INTERFACE)
if(FACILITY_CAPACITY_RESERVATION_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(facility_capacity_reservation_sanitizers INTERFACE /fsanitize=address)
    target_link_options(facility_capacity_reservation_sanitizers INTERFACE /fsanitize=address)
  else()
    target_compile_options(facility_capacity_reservation_sanitizers INTERFACE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(facility_capacity_reservation_sanitizers INTERFACE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
  endif()
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(facility_capacity_reservation
  src/status.cpp
  src/text.cpp
  src/strong_id.cpp
  src/digest.cpp
  src/capacity.cpp
  src/reservation.cpp
  src/ledger.cpp
  src/ledger_ops.cpp
  src/codec.cpp
  src/clock.cpp
  src/store.cpp
  src/file_ops.cpp
  src/detail/ledger_internal.cpp
  src/detail/fault.cpp)
add_library(dccp::facility_capacity_reservation ALIAS facility_capacity_reservation)

target_compile_features(facility_capacity_reservation PUBLIC cxx_std_20)
target_include_directories(facility_capacity_reservation
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(facility_capacity_reservation
  PRIVATE
    $<BUILD_INTERFACE:facility_capacity_reservation_warnings>
    $<BUILD_INTERFACE:facility_capacity_reservation_sanitizers>)
set_target_properties(facility_capacity_reservation PROPERTIES
  CXX_EXTENSIONS OFF
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON
  EXPORT_NAME facility_capacity_reservation)

# ---------------------------------------------------------------------------
# Inspection tool
# ---------------------------------------------------------------------------

if(FACILITY_CAPACITY_RESERVATION_BUILD_TOOLS)
  add_executable(fcrctl tools/fcrctl/main.cpp)
  target_link_libraries(fcrctl PRIVATE
    dccp::facility_capacity_reservation
    facility_capacity_reservation_warnings
    facility_capacity_reservation_sanitizers)
  set_target_properties(fcrctl PROPERTIES CXX_EXTENSIONS OFF)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

if(FACILITY_CAPACITY_RESERVATION_BUILD_EXAMPLES)
  foreach(example_name IN ITEMS
      basic_reservation
      durable_lifecycle
      reconcile_capacity)
    add_executable(facility_capacity_reservation_example_${example_name} examples/${example_name}.cpp)
    target_link_libraries(facility_capacity_reservation_example_${example_name} PRIVATE
      dccp::facility_capacity_reservation
      facility_capacity_reservation_warnings
      facility_capacity_reservation_sanitizers)
    set_target_properties(facility_capacity_reservation_example_${example_name} PROPERTIES CXX_EXTENSIONS OFF)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(FACILITY_CAPACITY_RESERVATION_BUILD_TESTS)
  enable_testing()

  add_library(facility_capacity_reservation_test_support STATIC
    tests/test_main.cpp
    tests/test_support.cpp
    tests/tests_multiprocess_child.cpp
    tests/child_process.cpp)
  target_link_libraries(facility_capacity_reservation_test_support
    PUBLIC dccp::facility_capacity_reservation
           facility_capacity_reservation_warnings
           facility_capacity_reservation_sanitizers)
  target_include_directories(facility_capacity_reservation_test_support
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  # The test suite compares the compiled-in library version with the CMake
  # project version so the two cannot drift apart silently.
  target_compile_definitions(facility_capacity_reservation_test_support
    PUBLIC FACILITY_CAPACITY_RESERVATION_CMAKE_VERSION="${PROJECT_VERSION}")
  set_target_properties(facility_capacity_reservation_test_support PROPERTIES CXX_EXTENSIONS OFF)

  add_executable(facility_capacity_reservation_tests
    tests/tests_status.cpp
    tests/tests_text.cpp
    tests/tests_digest.cpp
    tests/tests_ids.cpp
    tests/tests_capacity.cpp
    tests/tests_ledger_basics.cpp
    tests/tests_ledger_accounting.cpp
    tests/tests_ledger_atomicity.cpp
    tests/tests_ledger_lifecycle.cpp
    tests/tests_ledger_idempotency.cpp
    tests/tests_ledger_reconcile.cpp
    tests/tests_ledger_revalidate.cpp
    tests/tests_codec.cpp
    tests/tests_store.cpp
    tests/tests_recovery.cpp
    tests/tests_corruption.cpp
    tests/tests_property.cpp
    tests/tests_adversarial.cpp
    tests/tests_concurrency.cpp
    tests/tests_multiprocess.cpp
    tests/tests_internal.cpp
    tests/tests_cli.cpp)
  target_link_libraries(facility_capacity_reservation_tests PRIVATE facility_capacity_reservation_test_support)
  target_include_directories(facility_capacity_reservation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  set_target_properties(facility_capacity_reservation_tests PROPERTIES CXX_EXTENSIONS OFF)

  target_compile_definitions(facility_capacity_reservation_tests
    PRIVATE FACILITY_CAPACITY_RESERVATION_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  if(FACILITY_CAPACITY_RESERVATION_BUILD_TOOLS)
    target_compile_definitions(facility_capacity_reservation_tests
      PRIVATE FACILITY_CAPACITY_RESERVATION_CLI_PATH="$<TARGET_FILE:fcrctl>")
    add_dependencies(facility_capacity_reservation_tests fcrctl)
  endif()

  add_test(NAME facility_capacity_reservation_tests COMMAND facility_capacity_reservation_tests)
  set_tests_properties(facility_capacity_reservation_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  # An independent out-of-tree consumer of the installed package.
  add_test(NAME facility_capacity_reservation_downstream
    COMMAND ${CMAKE_COMMAND}
      -DCMAKE_COMMAND=${CMAKE_COMMAND}
      -DPROJECT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DCONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/downstream
      -DCONSUMER_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/downstream
      -DINSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/install-test
      -DCONFIG=$<CONFIG>
      -DGENERATOR=${CMAKE_GENERATOR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/downstream/run_consumer.cmake)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

if(FACILITY_CAPACITY_RESERVATION_BUILD_BENCHMARKS)
  add_executable(facility_capacity_reservation_benchmarks benchmarks/benchmarks.cpp)
  target_link_libraries(facility_capacity_reservation_benchmarks PRIVATE
    dccp::facility_capacity_reservation
    facility_capacity_reservation_warnings
    facility_capacity_reservation_sanitizers)
  set_target_properties(facility_capacity_reservation_benchmarks PROPERTIES CXX_EXTENSIONS OFF)
endif()

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------

install(TARGETS facility_capacity_reservation
  EXPORT facility_capacity_reservationTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(FACILITY_CAPACITY_RESERVATION_BUILD_TOOLS)
  install(TARGETS fcrctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(FACILITY_CAPACITY_RESERVATION_CMAKE_INSTALL_DIR
    ${CMAKE_INSTALL_LIBDIR}/cmake/facility_capacity_reservation)

install(EXPORT facility_capacity_reservationTargets
  FILE facility_capacity_reservationTargets.cmake
  NAMESPACE dccp::
  DESTINATION ${FACILITY_CAPACITY_RESERVATION_CMAKE_INSTALL_DIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/facility_capacity_reservationConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/facility_capacity_reservationConfig.cmake
  INSTALL_DESTINATION ${FACILITY_CAPACITY_RESERVATION_CMAKE_INSTALL_DIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/facility_capacity_reservationConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/facility_capacity_reservationConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/facility_capacity_reservationConfigVersion.cmake
  DESTINATION ${FACILITY_CAPACITY_RESERVATION_CMAKE_INSTALL_DIR})

install(FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/README.md
  ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
  ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE
  DESTINATION ${CMAKE_INSTALL_DOCDIR})
