// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Operation requests and their deterministic outcomes.
//
// Every request that depends on current authoritative state carries its exact
// preconditions: the writer authority epoch, the expected state revision and,
// where a specific reservation generation is being changed, the expected
// reservation generation. A precondition that no longer holds is refused with a
// specific code; it is never merged into current state.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_REQUEST_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_REQUEST_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dccp/facility_capacity_reservation/capacity.hpp"
#include "dccp/facility_capacity_reservation/reservation.hpp"
#include "dccp/facility_capacity_reservation/status.hpp"
#include "dccp/facility_capacity_reservation/strong_id.hpp"
#include "dccp/facility_capacity_reservation/units.hpp"

namespace dccp::facility_capacity_reservation {

/// Bounds applied to every request before anything is allocated or changed.
struct RequestLimits {
  std::size_t max_claims = 256;
  std::size_t max_lineage_entries = 1024;
  std::size_t max_detail_bytes = kMaxTextBytes;
  std::size_t max_expirations_per_sweep = 100'000;
};

/// Common preconditions shared by every mutating request.
struct AuthorityPreconditions {
  /// Writer authority epoch the caller believes it holds. Required.
  AuthorityEpoch epoch;

  /// Expected published state revision. When non-zero the exact revision is
  /// required; zero means "any revision".
  Revision revision;
};

/// A reservation the caller asks the facility to commit.
struct ReserveRequest {
  RequestLimits limits;
  AuthorityPreconditions authority;

  AttemptId attempt;
  ReservationId id;

  ClaimantRef claimant;
  TenantRef tenant;
  ServiceRef service;
  PriorityRef priority;
  HeadroomClass headroom = HeadroomClass::Guaranteed;

  std::vector<ResourceClaim> claims;
  ValidityInterval validity;

  /// Exact source generation of the capacity snapshot the caller priced this
  /// commitment against. A mismatch is refused: a commitment is never made
  /// against capacity evidence the caller did not see.
  SourceGeneration expected_source_generation;

  /// Current logical tick.
  Tick now;
};

/// Outcome of a successful reservation.
struct ReserveOutcome {
  ReservationView reservation;
  Revision revision;
  AuthorityEpoch epoch;
  /// True when the attempt identity replayed a previously published outcome.
  bool replayed = false;
};

/// Amends the head generation of an existing reservation.
///
/// The claim set, validity interval, priority and headroom class are replaced
/// wholesale: an amendment is never a partial edit. The amendment is atomic
/// across every pool, and the previous generation becomes history recorded in
/// the lineage.
struct AmendRequest {
  RequestLimits limits;
  AuthorityPreconditions authority;

  AttemptId attempt;
  ReservationId id;

  /// Exact generation the caller is amending.
  ReservationGeneration expected_generation;

  /// Actor performing the amendment. Must equal the record's claimant.
  ActorRef actor;

  std::vector<ResourceClaim> claims;
  ValidityInterval validity;
  std::optional<PriorityRef> priority;   // absent: keep the current priority
  std::optional<HeadroomClass> headroom; // absent: keep the current class

  /// Exact source generation the amended claims were priced against. Required,
  /// and required to equal the source generation the ledger currently holds:
  /// an amendment that changes what a reservation binds is a new commitment.
  SourceGeneration expected_source_generation;

  AmendmentCause cause = AmendmentCause::Correction;
  std::string detail;

  Tick now;
};

struct AmendOutcome {
  ReservationView reservation;
  ReservationGeneration previous_generation;
  Revision revision;
  AuthorityEpoch epoch;
  bool replayed = false;
};

/// Releases an active reservation before its deadline.
struct ReleaseRequest {
  RequestLimits limits;
  AuthorityPreconditions authority;

  AttemptId attempt;
  ReservationId id;
  ReservationGeneration expected_generation;
  ActorRef actor;
  std::string detail;
  Tick now;
};

struct ReleaseOutcome {
  ReservationView reservation;
  Revision revision;
  AuthorityEpoch epoch;
  /// Capacity returned to free, per pool, in canonical order.
  std::vector<ResourceClaim> released;
  bool replayed = false;
};

/// Revokes an active reservation by facility authority.
struct RevokeRequest {
  RequestLimits limits;
  AuthorityPreconditions authority;

  AttemptId attempt;
  ReservationId id;
  ReservationGeneration expected_generation;

  /// Actor performing the revocation.
  ActorRef actor;

  /// Policy that authorises the revocation. Required for Firm and
  /// Opportunistic headroom, and additionally for Guaranteed unless
  /// `allow_guaranteed_override` is set.
  std::optional<PolicyRef> policy;

  /// Explicit acknowledgement that a Guaranteed commitment is being revoked.
  /// Without it a Guaranteed revocation is refused.
  bool allow_guaranteed_override = false;

  TransitionCause cause = TransitionCause::AuthorityRevocation;
  std::string detail;
  Tick now;
};

struct RevokeOutcome {
  ReservationView reservation;
  Revision revision;
  AuthorityEpoch epoch;
  std::vector<ResourceClaim> reclaimed;
  bool replayed = false;
};

/// One reservation expired by a sweep.
struct ExpiredReservation {
  ReservationId id;
  ReservationGeneration generation;
  std::vector<ResourceClaim> reclaimed;
};

/// Expires every active reservation whose deadline has elapsed.
///
/// The sweep is deterministic: reservations are visited in identifier order and
/// at most `limits.max_expirations_per_sweep` are expired in one call.
struct ExpireRequest {
  RequestLimits limits;
  AuthorityPreconditions authority;
  AttemptId attempt;
  Tick now;
};

struct ExpireOutcome {
  std::vector<ExpiredReservation> expired;  // identifier order
  Revision revision;
  AuthorityEpoch epoch;
  bool replayed = false;
};

/// Classification of one reservation during revalidation.
enum class RevalidationClass : std::uint8_t {
  Current = 1,               // holds capacity that still exists and is current
  DeadlinePassed = 2,        // the validity interval has elapsed
  SourceGenerationStale = 3, // priced against an older capacity generation
  PoolMissing = 4,           // a claimed pool is absent from current capacity
  CapacityExceeded = 5,      // the claim is larger than the pool's reservable size
};

std::string_view revalidation_class_token(RevalidationClass value) noexcept;
Result<RevalidationClass> parse_revalidation_class(std::string_view token);

/// One revalidation finding.
struct RevalidationEntry {
  ReservationId id;
  ReservationGeneration generation;
  RevalidationClass classification = RevalidationClass::Current;
  std::string detail;
};

/// Re-checks the held state against current capacity evidence.
///
/// This is an observation, not a mutation: it publishes nothing and changes
/// nothing. It carries the exact revision it observed so that a report can never
/// be mistaken for a statement about a later state.
struct RevalidateRequest {
  RequestLimits limits;
  Revision expected_revision;
  Tick now;
};

struct RevalidationReport {
  Revision revision;
  Tick now;
  AuthorityEpoch epoch;
  SourceGeneration source_generation;
  std::vector<RevalidationEntry> entries;  // identifier order
  std::size_t current_count = 0;
  std::size_t stale_count = 0;
  std::size_t deadline_passed_count = 0;
  std::size_t capacity_exceeded_count = 0;
  std::size_t pool_missing_count = 0;
};

/// What reconciliation should do about capacity that shrank below commitments.
enum class ReconcileMode : std::uint8_t {
  /// Report the overcommit and change nothing.
  Observe = 1,
  /// Fence opportunistic and firm commitments, deterministically, until the
  /// accounting closes again. Guaranteed commitments are never fenced
  /// automatically: an overcommit that survives them is unresolvable.
  Enforce = 2,
};

std::string_view reconcile_mode_token(ReconcileMode value) noexcept;
Result<ReconcileMode> parse_reconcile_mode(std::string_view token);

/// One pool that is over-committed by the newly consumed capacity.
struct PoolOvercommit {
  PoolKey pool;
  Units reservable = 0;
  Units committed = 0;
  Units protected_ = 0;
  Units excess = 0;
};

/// One reservation fenced by reconciliation.
struct FencedReservation {
  ReservationId id;
  ReservationGeneration generation;
  HeadroomClass headroom = HeadroomClass::Guaranteed;
  TransitionCause cause = TransitionCause::CapacityWithdrawn;
};

/// Adopts a newly consumed capacity snapshot.
///
/// The adoption carries an exact precondition on the source generation the
/// ledger currently holds, so a snapshot produced from an older generation than
/// the one already consumed is refused rather than silently replacing newer
/// evidence.
struct ReconcileRequest {
  RequestLimits limits;
  AuthorityPreconditions authority;
  AttemptId attempt;

  /// Capacity evidence to adopt. Must declare a source generation strictly newer
  /// than the one currently held.
  CapacitySnapshot snapshot;

  /// The exact source generation the caller believes the ledger holds.
  SourceGeneration expected_source_generation;

  ReconcileMode mode = ReconcileMode::Observe;
  Tick now;

  /// Actor performing the reconciliation. Recorded in the provenance of every
  /// reservation fenced by it.
  ActorRef actor;

  /// Policy that authorises fencing capacity out from under its claimants.
  /// Required in Enforce mode when at least one reservation would be fenced.
  std::optional<PolicyRef> policy;
};

struct ReconcileOutcome {
  Revision revision;
  AuthorityEpoch epoch;
  SourceGeneration previous_source_generation;
  SourceGeneration source_generation;
  std::vector<PoolOvercommit> overcommits;
  std::vector<FencedReservation> fenced;
  bool adopted = false;
  bool replayed = false;
};

/// Verification of the published accounting against an independent recomputation.
struct VerificationReport {
  Revision revision;
  AuthorityEpoch epoch;
  SourceGeneration source_generation;
  std::size_t reservation_count = 0;
  std::size_t active_count = 0;
  std::size_t attempt_count = 0;
  std::size_t pool_count = 0;
  /// Pools whose recorded split did not equal the independently recomputed one.
  std::vector<PoolKey> mismatched_pools;
  /// True when every pool closed exactly and every record satisfied every
  /// structural invariant.
  bool ok = false;
};

/// A point-in-time account of one pool.
struct PoolAccount {
  PoolKey pool;
  Units gross = 0;
  Units withdrawn = 0;
  Units floor = 0;
  Units available = 0;
  Units reservable = 0;
  Units committed = 0;
  Units protected_ = 0;
  Units free = 0;
  std::size_t active_reservations = 0;
};

/// Status of a ledger or store.
struct LedgerStatus {
  Revision revision;
  AuthorityEpoch epoch;
  Incarnation incarnation;
  SourceGeneration source_generation;
  SnapshotRef source_snapshot;
  FacilityRef facility;
  /// True when the held capacity evidence was installed fresh in this process
  /// rather than restored from persistence.
  bool capacity_fresh = false;
  bool durable = false;
  bool closed = false;
  std::size_t reservation_count = 0;
  std::size_t active_count = 0;
  std::size_t attempt_count = 0;
  std::size_t pool_count = 0;
};

}  // namespace dccp::facility_capacity_reservation

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_REQUEST_HPP
