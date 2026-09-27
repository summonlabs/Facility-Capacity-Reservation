// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Logical tick source.
//
// The library never derives a tick of its own: every request carries the tick it
// was made at, which is what makes the lifecycle deterministic and testable. This
// header exists for callers — the examples, the inspection tool and the durable
// store's own bookkeeping — that need a reasonable default.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_CLOCK_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_CLOCK_HPP

#include <cstdint>

#include "dccp/facility_capacity_reservation/status.hpp"
#include "dccp/facility_capacity_reservation/strong_id.hpp"

namespace dccp::facility_capacity_reservation {

/// A monotonic logical tick expressed in milliseconds since the Unix epoch.
///
/// The value is derived from the system clock, which is not monotonic across
/// adjustments; the library therefore treats ticks as caller-supplied labels and
/// never relies on them moving forward on their own. A store's most recent
/// observed tick is persisted, so validity intervals remain comparable across
/// restarts.
class SystemTickSource {
 public:
  /// Current tick. Never returns zero: a zero tick is reserved for "absent".
  Tick now() const noexcept;
};

/// Convenience: the current system tick.
Tick system_now_tick() noexcept;

/// Milliseconds since the Unix epoch, or zero before 1970 (which cannot occur on
/// the platforms this project is exercised on).
std::uint64_t unix_milliseconds() noexcept;

}  // namespace dccp::facility_capacity_reservation

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_CLOCK_HPP
