# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# An independent out-of-tree consumer of the installed package. It is built with
# find_package against a real installation prefix and is deliberately NOT part of
# this project's build graph.

cmake_minimum_required(VERSION 3.20)

project(facility_capacity_reservation_consumer LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(facility_capacity_reservation 1.0 REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE dccp::facility_capacity_reservation)

# The consumer must be able to compile against the installed headers alone: it
# includes no private header and links nothing but the exported target.
if(MSVC)
  target_compile_options(consumer PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc)
endif()
