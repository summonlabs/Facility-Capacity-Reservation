// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Exact integer capacity units and checked arithmetic.
//
// All authoritative capacity accounting is performed on std::uint64_t units.
// There is no floating-point capacity anywhere in this library: a commitment is
// an exact integer count of a unit whose meaning is fixed by the resource kind.
// Every arithmetic step is checked, and overflow is reported as an error rather
// than wrapped.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_UNITS_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_UNITS_HPP

#include <cstdint>
#include <limits>

#include "dccp/facility_capacity_reservation/status.hpp"

namespace dccp::facility_capacity_reservation {

using Units = std::uint64_t;

inline constexpr Units kMaxUnits = std::numeric_limits<Units>::max();

/// Checked addition: reports overflow instead of wrapping.
inline Result<Units> checked_add(Units lhs, Units rhs) {
  if (rhs > kMaxUnits - lhs) {
    return Error(ErrorCode::ArithmeticOverflow, "capacity addition overflowed");
  }
  return lhs + rhs;
}

/// Checked subtraction: reports underflow instead of wrapping.
inline Result<Units> checked_sub(Units lhs, Units rhs) {
  if (rhs > lhs) {
    return Error(ErrorCode::ArithmeticOverflow, "capacity subtraction underflowed");
  }
  return lhs - rhs;
}

/// Checked multiplication: reports overflow instead of wrapping.
inline Result<Units> checked_mul(Units lhs, Units rhs) {
  if (lhs != 0 && rhs > kMaxUnits / lhs) {
    return Error(ErrorCode::ArithmeticOverflow, "capacity multiplication overflowed");
  }
  return lhs * rhs;
}

/// Accumulates `amount` into `total`, reporting the overflow.
inline Result<void> checked_accumulate(Units& total, Units amount) {
  FCR_TRY(sum, checked_add(total, amount));
  total = sum;
  return ok();
}

/// A count of units of one resource kind.
///
/// The unit is fixed by the resource kind and is never converted: a unit of
/// space capacity and a watt of power capacity are different quantities and are
/// never added together. Zero is a legal quantity only where the model says so
/// (a pool may have a zero floor); a claim may not.
class Quantity {
 public:
  constexpr Quantity() = default;
  explicit constexpr Quantity(Units units) noexcept : units_(units) {}

  static constexpr Quantity zero() noexcept { return Quantity(0); }

  constexpr Units units() const noexcept { return units_; }
  constexpr bool is_zero() const noexcept { return units_ == 0; }

  friend constexpr bool operator==(Quantity lhs, Quantity rhs) noexcept { return lhs.units_ == rhs.units_; }
  friend constexpr bool operator!=(Quantity lhs, Quantity rhs) noexcept { return !(lhs == rhs); }
  friend constexpr bool operator<(Quantity lhs, Quantity rhs) noexcept { return lhs.units_ < rhs.units_; }
  friend constexpr bool operator<=(Quantity lhs, Quantity rhs) noexcept { return lhs.units_ <= rhs.units_; }
  friend constexpr bool operator>(Quantity lhs, Quantity rhs) noexcept { return rhs < lhs; }
  friend constexpr bool operator>=(Quantity lhs, Quantity rhs) noexcept { return rhs <= lhs; }

 private:
  Units units_ = 0;
};

/// A closeable capacity triple: available space split into free, committed and
/// protected units.
///
/// "Committed" is capacity held by reservations that cannot be reclaimed by the
/// facility without an explicit revocation. "Protected" is capacity held by
/// opportunistic reservations: it is accounted separately so that a facility
/// can see how much of its committed footprint is reclaimable. Both are durable
/// commitments; neither is free.
struct CapacitySplit {
  Quantity free;
  Quantity committed;
  Quantity protected_;

  /// Total accounted capacity. Checked: a triple that does not fit is a defect,
  /// not a silently truncated value.
  Result<Quantity> accounted() const {
    FCR_TRY(held, checked_add(committed.units(), protected_.units()));
    FCR_TRY(total, checked_add(held, free.units()));
    return Quantity(total);
  }
};

}  // namespace dccp::facility_capacity_reservation

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_UNITS_HPP
