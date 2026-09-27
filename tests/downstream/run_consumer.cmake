# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Installs the project into a scratch prefix, builds an independent consumer
# against the installed package with find_package, and runs it.
#
# The consumer source directory is deliberately outside this project's build
# graph: nothing about the consumer is configured until the package is installed.

if(NOT DEFINED CMAKE_COMMAND OR NOT DEFINED PROJECT_BINARY_DIR OR NOT DEFINED CONSUMER_SOURCE_DIR OR
   NOT DEFINED CONSUMER_BINARY_DIR OR NOT DEFINED INSTALL_PREFIX)
  message(FATAL_ERROR
          "run_consumer.cmake requires CMAKE_COMMAND, PROJECT_BINARY_DIR, CONSUMER_SOURCE_DIR, CONSUMER_BINARY_DIR and INSTALL_PREFIX")
endif()

get_filename_component(project_binary_dir "${PROJECT_BINARY_DIR}" ABSOLUTE)

set(config_args "")
if(CONFIG AND NOT CONFIG STREQUAL "")
  set(config_args --config "${CONFIG}")
endif()

file(REMOVE_RECURSE "${INSTALL_PREFIX}")
file(REMOVE_RECURSE "${CONSUMER_BINARY_DIR}")

message(STATUS "installing into ${INSTALL_PREFIX}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${project_binary_dir}" --prefix "${INSTALL_PREFIX}" ${config_args}
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "install failed (${install_result})\n${install_output}\n${install_error}")
endif()

# The installed package must carry the headers, the library, the export set and
# the version file.
foreach(required IN ITEMS
    "include/dccp/facility_capacity_reservation/store.hpp"
    "include/dccp/facility_capacity_reservation/status.hpp"
    "lib/cmake/facility_capacity_reservation/facility_capacity_reservationConfig.cmake"
    "lib/cmake/facility_capacity_reservation/facility_capacity_reservationConfigVersion.cmake"
    "lib/cmake/facility_capacity_reservation/facility_capacity_reservationTargets.cmake"
    "share/doc/facility_capacity_reservation/LICENSE"
    "share/doc/facility_capacity_reservation/NOTICE")
  if(NOT EXISTS "${INSTALL_PREFIX}/${required}")
    message(FATAL_ERROR "the installation is missing ${required}")
  endif()
endforeach()

message(STATUS "configuring the out-of-tree consumer")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -S "${CONSUMER_SOURCE_DIR}"
          -B "${CONSUMER_BINARY_DIR}"
          "-DCMAKE_PREFIX_PATH=${INSTALL_PREFIX}"
          "-DCMAKE_BUILD_TYPE=Release"
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "consumer configure failed (${configure_result})\n${configure_output}\n${configure_error}")
endif()

message(STATUS "building the consumer")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${CONSUMER_BINARY_DIR}" ${config_args}
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "consumer build failed (${build_result})\n${build_output}\n${build_error}")
endif()

set(consumer_exe "${CONSUMER_BINARY_DIR}/consumer")
if(CONFIG AND NOT CONFIG STREQUAL "" AND EXISTS "${CONSUMER_BINARY_DIR}/${CONFIG}/consumer.exe")
  set(consumer_exe "${CONSUMER_BINARY_DIR}/${CONFIG}/consumer.exe")
elseif(EXISTS "${CONSUMER_BINARY_DIR}/consumer.exe")
  set(consumer_exe "${CONSUMER_BINARY_DIR}/consumer.exe")
elseif(EXISTS "${CONSUMER_BINARY_DIR}/${CONFIG}/consumer")
  set(consumer_exe "${CONSUMER_BINARY_DIR}/${CONFIG}/consumer")
endif()

message(STATUS "running the consumer")
execute_process(
  COMMAND "${consumer_exe}" "${CONSUMER_BINARY_DIR}/consumer-store"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
message(STATUS "consumer output:\n${run_output}")
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "consumer run failed (${run_result})\n${run_output}\n${run_error}")
endif()

message(STATUS "downstream consumer verified against ${INSTALL_PREFIX}")
