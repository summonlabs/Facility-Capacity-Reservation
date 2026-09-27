// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Publish-path fault injection.
//
// The durable publication protocol is only meaningful if it survives the process
// dying at each step of it. That cannot be proved by reasoning alone, so the
// protocol exposes one documented seam: when the environment variable
// FCR_FAULT_INJECT names a stage, the publishing process terminates abruptly at
// that stage — no unwinding, no cleanup, no destructor, exactly like a crash.
//
// The seam is inert unless the variable is set. It carries no product behaviour
// and no state of its own.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_DETAIL_FAULT_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_DETAIL_FAULT_HPP

#include <string_view>

namespace dccp::facility_capacity_reservation::detail {

/// Named stages of the publication protocol.
enum class PublishStage {
  None = 0,
  /// After the new state generation has been written and verified, before the
  /// head pointer has been replaced.
  AfterStateWrite = 1,
  /// Immediately before the head pointer is replaced.
  BeforeHeadCommit = 2,
  /// After the head pointer has been replaced, before superseded generations are
  /// retired.
  AfterHeadCommit = 3,
};

/// Environment variable that selects a fault stage.
inline constexpr std::string_view kFaultEnvironmentVariable = "FCR_FAULT_INJECT";

/// Exit status used when a configured fault fires. Distinct from every status the
/// library returns so a test can tell a crash from a rejection.
inline constexpr int kFaultExitStatus = 70;

/// Parses a stage token. Unknown tokens select None.
PublishStage parse_publish_stage(std::string_view token) noexcept;

/// The configured stage, read once per process.
PublishStage configured_publish_fault() noexcept;

/// Returns normally unless the configured stage is `stage`, in which case the
/// process terminates immediately without unwinding.
void reach_publish_stage(PublishStage stage) noexcept;

}  // namespace dccp::facility_capacity_reservation::detail

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_DETAIL_FAULT_HPP
