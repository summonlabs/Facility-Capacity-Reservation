// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity_reservation/clock.hpp"

#include <chrono>

namespace dccp::facility_capacity_reservation {

std::uint64_t unix_milliseconds() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  if (milliseconds <= 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(milliseconds);
}

Tick SystemTickSource::now() const noexcept {
  const std::uint64_t milliseconds = unix_milliseconds();
  // A zero tick means "absent"; the epoch itself maps to one.
  return Tick(milliseconds == 0 ? 1 : milliseconds);
}

Tick system_now_tick() noexcept { return SystemTickSource().now(); }

}  // namespace dccp::facility_capacity_reservation
