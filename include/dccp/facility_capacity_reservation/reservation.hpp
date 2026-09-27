// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The reservation model: identity, generations, claims, headroom, validity,
// lifecycle, amendment lineage and provenance.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_RESERVATION_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_RESERVATION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_capacity_reservation/capacity.hpp"
#include "dccp/facility_capacity_reservation/status.hpp"
#include "dccp/facility_capacity_reservation/strong_id.hpp"
#include "dccp/facility_capacity_reservation/units.hpp"

namespace dccp::facility_capacity_reservation {

/// Headroom class of a commitment.
///
/// The class decides which accounting bucket a commitment occupies and whether
/// the facility may reclaim it unilaterally.
enum class HeadroomClass : std::uint8_t {
  /// Counted as committed. May only end through release, expiry or amendment by
  /// its claimant. A revocation requires an explicit policy override.
  Guaranteed = 1,
  /// Counted as committed. The facility may revoke it against a policy
  /// reference without the claimant's consent.
  Firm = 2,
  /// Counted as protected, not committed. Reclaimable at any time; it is the
  /// capacity a facility sells twice.
  Opportunistic = 3,
};

std::string_view headroom_class_token(HeadroomClass value) noexcept;
Result<HeadroomClass> parse_headroom_class(std::string_view token);
std::string_view headroom_class_name(HeadroomClass value) noexcept;

/// True when commitments of this class are accounted as committed rather than
/// protected.
constexpr bool headroom_counts_as_committed(HeadroomClass value) noexcept {
  return value != HeadroomClass::Opportunistic;
}

/// Lifecycle state of a reservation generation that is currently the head of
/// its identity. Superseded generations are not head records: they are recorded
/// in the amendment lineage instead.
enum class ReservationState : std::uint8_t {
  Active = 1,
  Released = 2,
  Expired = 3,
  Revoked = 4,
};

std::string_view reservation_state_token(ReservationState value) noexcept;
Result<ReservationState> parse_reservation_state(std::string_view token);
std::string_view reservation_state_name(ReservationState value) noexcept;

/// True when the state holds capacity.
constexpr bool reservation_state_holds_capacity(ReservationState value) noexcept {
  return value == ReservationState::Active;
}

/// True when the state is final for the generation.
constexpr bool reservation_state_is_terminal(ReservationState value) noexcept {
  return value != ReservationState::Active;
}

/// Why a lifecycle transition happened. Recorded as provenance, never inferred.
enum class TransitionCause : std::uint8_t {
  ClaimantRequest = 1,     // the claimant asked for it
  DeadlineElapsed = 2,     // the validity interval ran out
  AuthorityRevocation = 3, // the facility revoked it
  CapacityWithdrawn = 4,   // reconciliation removed the capacity it held
  Superseded = 5,          // an amendment replaced this generation
  FacilityOverride = 6,    // a policy override forced the transition
};

std::string_view transition_cause_token(TransitionCause value) noexcept;
Result<TransitionCause> parse_transition_cause(std::string_view token);
std::string_view transition_cause_name(TransitionCause value) noexcept;

/// Why an amendment was made. Machine-readable cause plus a bounded free-text
/// detail; the detail is never used for decisions.
enum class AmendmentCause : std::uint8_t {
  Correction = 1,
  CapacityIncrease = 2,
  CapacityDecrease = 3,
  ScopeChange = 4,
  DeadlineExtension = 5,
  DeadlineReduction = 6,
  PriorityChange = 7,
  HeadroomChange = 8,
  Other = 9,
};

std::string_view amendment_cause_token(AmendmentCause value) noexcept;
Result<AmendmentCause> parse_amendment_cause(std::string_view token);
std::string_view amendment_cause_name(AmendmentCause value) noexcept;

/// A half-open validity interval [start, deadline).
///
/// The deadline is exclusive: at tick == deadline the reservation no longer
/// holds capacity. Intervals are compared and validated with checked integer
/// arithmetic, and the domain is the producer's monotonic logical tick.
struct ValidityInterval {
  Tick start;
  Tick deadline;

  Result<void> validate() const {
    if (start.is_zero()) {
      return Error(ErrorCode::InvalidInterval, "validity start tick must not be zero");
    }
    if (deadline.is_zero()) {
      return Error(ErrorCode::InvalidInterval, "validity deadline tick must not be zero");
    }
    if (deadline <= start) {
      return Error(ErrorCode::InvalidInterval, "validity deadline must be strictly after the start tick");
    }
    return ok();
  }

  /// True when `now` is inside the half-open interval.
  bool contains(Tick now) const noexcept { return start <= now && now < deadline; }

  /// True when the interval has elapsed at `now`.
  bool elapsed_at(Tick now) const noexcept { return now >= deadline; }

  std::uint64_t duration_ticks() const noexcept { return deadline.value() - start.value(); }
};

/// One typed claim: an exact amount of one capacity pool.
struct ResourceClaim {
  PoolKey pool;
  Units amount = 0;

  friend bool operator==(const ResourceClaim& lhs, const ResourceClaim& rhs) noexcept {
    return lhs.pool == rhs.pool && lhs.amount == rhs.amount;
  }
  friend bool operator!=(const ResourceClaim& lhs, const ResourceClaim& rhs) noexcept { return !(lhs == rhs); }
  friend bool operator<(const ResourceClaim& lhs, const ResourceClaim& rhs) noexcept {
    if (lhs.pool != rhs.pool) {
      return lhs.pool < rhs.pool;
    }
    return lhs.amount < rhs.amount;
  }
};

/// One entry in a reservation's amendment lineage.
struct AmendmentEntry {
  ReservationGeneration generation;   // the generation an amendment produced
  ReservationGeneration predecessor;  // the generation it replaced
  AttemptId attempt;
  Revision revision;                  // state revision that published it
  AuthorityEpoch epoch;               // authority that published it
  Tick at_tick;
  AmendmentCause cause = AmendmentCause::Other;
  std::string detail;  // bounded, encoded as a canonical field
};

/// Provenance of a terminal transition. Present exactly when the head record is
/// not active.
struct TerminationProvenance {
  TransitionCause cause = TransitionCause::ClaimantRequest;
  ActorRef actor;
  AttemptId attempt;
  Revision revision;
  AuthorityEpoch epoch;
  Tick at_tick;
  std::optional<PolicyRef> policy;  // present when a policy override applied
  std::string detail;
};

/// A reservation generation.
///
/// The identity (ReservationId) is separate from the metadata below: amending a
/// reservation mints a new generation of the same identity and never rewrites
/// history.
struct ReservationRecord {
  ReservationId id;
  ReservationGeneration generation;
  AuthorityEpoch authority_epoch;  // authority that minted this generation
  Revision revision;               // state revision that made it current
  AttemptId last_attempt;          // attempt that produced this generation

  ClaimantRef claimant;
  TenantRef tenant;
  ServiceRef service;
  PriorityRef priority;
  HeadroomClass headroom = HeadroomClass::Guaranteed;

  std::vector<ResourceClaim> claims;  // canonical order, unique pool, amount > 0

  SnapshotRef source_snapshot;
  SourceGeneration source_generation;

  ValidityInterval validity;
  ReservationState state = ReservationState::Active;

  Tick created_at_tick;
  Tick updated_at_tick;

  std::optional<TerminationProvenance> termination;
  std::vector<AmendmentEntry> lineage;  // oldest first, bounded

  /// Total claimed units in pools of one resource kind.
  Result<Units> claimed_of_kind(ResourceKind kind) const;

  /// Number of claims.
  std::size_t claim_count() const noexcept { return claims.size(); }

  /// The amount this record claims from `key`, or zero when it claims nothing.
  Units claimed_from(const PoolKey& key) const;
};

/// A read-only projection of a reservation for callers and inspection tools.
///
/// A view is a copy: it stays valid and unchanged after the store that produced
/// it has accepted further mutations.
struct ReservationView {
  ReservationRecord record;

  /// True when the record's source generation is older than the capacity
  /// snapshot the ledger currently holds. A stale reservation is still active;
  /// it simply has not been re-validated against current capacity evidence.
  bool source_stale = false;

  /// True when the record's validity interval has elapsed at the ledger's
  /// current tick but the record has not yet been expired.
  bool deadline_passed = false;

  const ReservationId& id() const noexcept { return record.id; }
  ReservationState state() const noexcept { return record.state; }
  bool active() const noexcept { return record.state == ReservationState::Active; }
};

}  // namespace dccp::facility_capacity_reservation

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_RESERVATION_HPP
