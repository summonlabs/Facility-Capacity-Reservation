// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Compiled-in version of the library. The CMake project version and this header
// are compared by the test suite so they cannot drift apart silently.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_VERSION_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace dccp::facility_capacity_reservation {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

/// "1.0.0"
inline constexpr std::string_view kVersionString = "1.0.0";

/// Numeric version as major * 10000 + minor * 100 + patch.
inline constexpr std::uint32_t kVersionNumber =
    kVersionMajor * 10000U + kVersionMinor * 100U + kVersionPatch;

/// Name of the runtime this repository provides within DCCP.
inline constexpr std::string_view kRuntimeName = "facility-capacity-reservation";

/// DCCP tranche and position of this runtime, as recorded in the NOTICE file.
inline constexpr std::string_view kDccpTranche = "Tranche 2 (Facility Capacity and Placement)";
inline constexpr std::string_view kDccpRepositoryIndex = "15 of 72";

}  // namespace dccp::facility_capacity_reservation

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_VERSION_HPP
