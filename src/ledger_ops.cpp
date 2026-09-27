// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The staged operations of the reservation ledger.
//
// Each operation applies the same ordered checks:
//
//   1. request shape            identifiers, limits, claim structure, interval
//   2. attempt identity         new, replayable or conflicting
//   3. writer authority epoch
//   4. expected state revision
//   5. subject existence and bounds
//   6. expected reservation generation
//   7. reservation lifecycle state
//   8. capability of the caller (claimant match, revocation policy)
//   9. claim-set change
//  10. capacity evidence        installed, fresh, exact source generation
//  11. capacity sufficiency     free units in every claimed pool
//
// Steps 1-9 are pure or read-only; 10-11 read evidence; nothing is written until
// every check has passed. Because the caller passes a scratch copy, a failure
// leaves the authoritative ledger untouched: that is what makes multi-resource
// atomicity and crash-safe publication fall out of the same structure.

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "detail/ledger_internal.hpp"
#include "dccp/facility_capacity_reservation/digest.hpp"
#include "dccp/facility_capacity_reservation/ledger.hpp"
#include "dccp/facility_capacity_reservation/text.hpp"

namespace dccp::facility_capacity_reservation {
namespace {

Result<void> require_ref(std::string_view value, std::string_view what) {
  if (value.empty()) {
    return Error(ErrorCode::MissingField, std::string(what) + " must not be empty");
  }
  return validate_identifier(value, what);
}

Result<void> require_attempt(const AttemptId& attempt) {
  if (attempt.empty()) {
    return Error(ErrorCode::AttemptRequired, "every mutation must carry an attempt identity");
  }
  return validate_identifier(attempt.value(), "attempt id");
}

/// The bound that actually applies to a request.
///
/// A request may tighten a bound but never relax it: the ledger's configured
/// limits are the ceiling, and a request's values are used only where they are
/// smaller. A caller therefore cannot talk the library into a larger claim set,
/// a longer lineage or a bigger sweep than the operator configured.
RequestLimits effective_limits(const RequestLimits& configured, const RequestLimits& requested) {
  RequestLimits limits;
  limits.max_claims = std::min(configured.max_claims, requested.max_claims);
  limits.max_lineage_entries = std::min(configured.max_lineage_entries, requested.max_lineage_entries);
  limits.max_detail_bytes = std::min(configured.max_detail_bytes, requested.max_detail_bytes);
  limits.max_expirations_per_sweep =
      std::min(configured.max_expirations_per_sweep, requested.max_expirations_per_sweep);
  return limits;
}

Result<void> check_authority(AuthorityEpoch current_epoch, Revision current_revision,
                             const AuthorityPreconditions& preconditions) {
  FCR_TRYV(detail::require_epoch(preconditions.epoch));
  if (preconditions.epoch != current_epoch) {
    return Error(ErrorCode::StaleAuthorityEpoch,
                 "the request was issued under a writer authority epoch that is no longer current")
        .with_subject("expected=" + format_unsigned(preconditions.epoch.value()) +
                      " current=" + format_unsigned(current_epoch.value()));
  }
  if (!preconditions.revision.is_zero() && preconditions.revision != current_revision) {
    return Error(ErrorCode::StaleRevision, "the request was issued against a state revision that has moved on")
        .with_subject("expected=" + format_unsigned(preconditions.revision.value()) +
                      " current=" + format_unsigned(current_revision.value()));
  }
  return ok();
}

bool same_validity(const ValidityInterval& lhs, const ValidityInterval& rhs) noexcept {
  return lhs.start == rhs.start && lhs.deadline == rhs.deadline;
}

Result<void> validate_headroom(HeadroomClass headroom) {
  switch (headroom) {
    case HeadroomClass::Guaranteed:
    case HeadroomClass::Firm:
    case HeadroomClass::Opportunistic:
      return ok();
  }
  return Error(ErrorCode::InvalidArgument, "the request names an unknown headroom class");
}

Result<void> validate_transition_cause(TransitionCause cause) {
  switch (cause) {
    case TransitionCause::ClaimantRequest:
    case TransitionCause::DeadlineElapsed:
    case TransitionCause::AuthorityRevocation:
    case TransitionCause::CapacityWithdrawn:
    case TransitionCause::Superseded:
    case TransitionCause::FacilityOverride:
      return ok();
  }
  return Error(ErrorCode::InvalidArgument, "the request names an unknown transition cause");
}

Result<void> validate_amendment_cause(AmendmentCause cause) {
  switch (cause) {
    case AmendmentCause::Correction:
    case AmendmentCause::CapacityIncrease:
    case AmendmentCause::CapacityDecrease:
    case AmendmentCause::ScopeChange:
    case AmendmentCause::DeadlineExtension:
    case AmendmentCause::DeadlineReduction:
    case AmendmentCause::PriorityChange:
    case AmendmentCause::HeadroomChange:
    case AmendmentCause::Other:
      return ok();
  }
  return Error(ErrorCode::InvalidArgument, "the request names an unknown amendment cause");
}

Result<void> validate_reconcile_mode(ReconcileMode mode) {
  switch (mode) {
    case ReconcileMode::Observe:
    case ReconcileMode::Enforce:
      return ok();
  }
  return Error(ErrorCode::InvalidArgument, "the request names an unknown reconciliation mode");
}

/// Capacity evidence must be installed, must have been consumed in this process
/// rather than restored from persistence, and must be the exact generation the
/// caller priced against.
///
/// This is the point at which "capacity" is separated from "authority to consume
/// it": restored evidence is real capacity, but it is no longer known to be
/// current, so it can hold and release commitments without authorising new ones.
Result<void> require_consumable_capacity(const ReservationLedger& ledger, SourceGeneration expected) {
  if (!ledger.has_capacity()) {
    return Error(ErrorCode::NoCapacityInstalled,
                 "no capacity evidence is installed; a capacity snapshot must be consumed first");
  }
  if (!ledger.capacity_fresh()) {
    return Error(ErrorCode::CapacityEvidenceStale,
                 "the held capacity evidence was restored from persistence and is not known to be current")
        .with_subject(ledger.capacity().ref.value());
  }
  if (expected.is_zero()) {
    return Error(ErrorCode::MissingField, "the request must carry the source generation it priced against");
  }
  if (expected != ledger.capacity().source_generation) {
    return Error(ErrorCode::SourceGenerationStale,
                 "the request was priced against a capacity source generation that is not the one held")
        .with_subject("expected=" + format_unsigned(expected.value()) +
                      " current=" + format_unsigned(ledger.capacity().source_generation.value()));
  }
  return ok();
}

/// Held units per pool, recomputed from the records.
///
/// Reconciliation derives its over-commit from this independent pass rather than
/// from the ledger's accounting map, so that a defect in the incremental
/// accounting cannot hide an over-commit.
Result<void> held_by_pool(const std::map<ReservationId, ReservationRecord>& records,
                          std::map<PoolKey, std::pair<Units, Units>>& out) {
  out.clear();
  for (const auto& entry : records) {
    const ReservationRecord& record = entry.second;
    if (!reservation_state_holds_capacity(record.state)) {
      continue;
    }
    const bool committed = headroom_counts_as_committed(record.headroom);
    for (const ResourceClaim& claim : record.claims) {
      auto position = out.find(claim.pool);
      if (position == out.end()) {
        position = out.emplace(claim.pool, std::make_pair(Units{0}, Units{0})).first;
      }
      Units& bucket = committed ? position->second.first : position->second.second;
      FCR_TRY(total, checked_add(bucket, claim.amount));
      bucket = total;
    }
  }
  return ok();
}

void overcommits_against(const std::map<PoolKey, std::pair<Units, Units>>& held, const CapacitySnapshot& snapshot,
                         std::vector<PoolOvercommit>& out) {
  out.clear();
  for (const auto& entry : held) {
    const CapacityPool* pool = snapshot.find(entry.first);
    Units reservable = 0;
    if (pool != nullptr) {
      const Result<Units> units = pool->reservable();
      reservable = units.has_value() ? *units : 0;
    }
    const Units committed = entry.second.first;
    const Units protected_units = entry.second.second;
    if (committed > kMaxUnits - protected_units) {
      continue;  // unreachable: records could not have been admitted
    }
    const Units wanted = committed + protected_units;
    if (wanted <= reservable) {
      continue;
    }
    PoolOvercommit overcommit;
    overcommit.pool = entry.first;
    overcommit.reservable = reservable;
    overcommit.committed = committed;
    overcommit.protected_ = protected_units;
    overcommit.excess = wanted - reservable;
    out.push_back(std::move(overcommit));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// reserve
// ---------------------------------------------------------------------------

Result<void> ReservationLedger::stage_reserve(const ReserveRequest& request, ReservationLedger& target,
                                              ReserveOutcome& outcome) {
  const RequestLimits limits = effective_limits(target.options_.limits, request.limits);

  FCR_TRYV(require_ref(request.id.value(), "reservation id"));
  FCR_TRYV(require_attempt(request.attempt));
  FCR_TRYV(require_ref(request.claimant.value(), "claimant reference"));
  FCR_TRYV(require_ref(request.tenant.value(), "tenant reference"));
  FCR_TRYV(require_ref(request.service.value(), "service reference"));
  FCR_TRYV(require_ref(request.priority.value(), "priority reference"));
  FCR_TRYV(validate_headroom(request.headroom));
  FCR_TRYV(request.validity.validate());
  if (request.now.is_zero()) {
    return Error(ErrorCode::MissingField, "a reservation requires the logical tick it is made at");
  }
  FCR_TRY(canonical, detail::canonical_claims(request.claims, limits));

  ReserveRequest normalized = request;
  normalized.claims = canonical;
  const std::string digest = intent_digest(normalized);

  const AttemptRecord* recorded = nullptr;
  switch (target.classify_attempt(request.attempt, OperationKind::Reserve, digest, recorded)) {
    case AttemptVerdict::Conflict:
      return Error(ErrorCode::AttemptConflict,
                   "this attempt identity was already used for a different request on this ledger")
          .with_subject(request.attempt.value());
    case AttemptVerdict::Replay: {
      const auto position = target.records_.find(request.id);
      if (position == target.records_.end()) {
        return Error(ErrorCode::AttemptNotReplayable,
                     "the attempt is recorded but the reservation it created is no longer held")
            .with_subject(request.attempt.value());
      }
      outcome.reservation = target.view_of(position->second);
      outcome.revision = target.revision_;
      outcome.epoch = target.epoch_;
      outcome.replayed = true;
      return ok();
    }
    case AttemptVerdict::New:
      break;
  }

  FCR_TRYV(check_authority(target.epoch_, target.revision_, request.authority));

  if (target.records_.find(request.id) != target.records_.end()) {
    return Error(ErrorCode::ReservationAlreadyExists, "a reservation with this identity is already held")
        .with_subject(request.id.value());
  }
  if (target.records_.size() >= target.options_.max_reservations) {
    return Error(ErrorCode::LimitExceeded, "the ledger already holds the configured maximum number of reservations");
  }

  FCR_TRYV(require_consumable_capacity(target, request.expected_source_generation));

  if (request.validity.elapsed_at(request.now)) {
    return Error(ErrorCode::DeadlineAlreadyPassed, "the requested validity interval has already elapsed")
        .with_subject(request.id.value());
  }

  // Sufficiency is checked for every pool before any pool is touched.
  for (const ResourceClaim& claim : canonical) {
    const auto position = target.accounting_.find(claim.pool);
    if (position == target.accounting_.end()) {
      return Error(ErrorCode::PoolUnknown, "the claim names a capacity pool that the held evidence does not declare")
          .with_subject(claim.pool.to_string());
    }
    if (claim.amount > position->second.free.units()) {
      return Error(ErrorCode::InsufficientCapacity,
                   "the pool does not have enough free capacity for this commitment")
          .with_subject(claim.pool.to_string() + " required=" + format_unsigned(claim.amount) +
                        " free=" + format_unsigned(position->second.free.units()));
    }
  }

  FCR_TRY(new_revision, target.revision_.next());

  ReservationRecord record;
  record.id = request.id;
  record.generation = ReservationGeneration::first();
  record.authority_epoch = target.epoch_;
  record.revision = new_revision;
  record.last_attempt = request.attempt;
  record.claimant = request.claimant;
  record.tenant = request.tenant;
  record.service = request.service;
  record.priority = request.priority;
  record.headroom = request.headroom;
  record.claims = canonical;
  record.source_snapshot = target.capacity_.ref;
  record.source_generation = target.capacity_.source_generation;
  record.validity = request.validity;
  record.state = ReservationState::Active;
  record.created_at_tick = request.now;
  record.updated_at_tick = request.now;

  target.records_.emplace(record.id, std::move(record));
  FCR_TRYV(target.rebuild_accounting());
  target.revision_ = new_revision;
  target.observe_tick(request.now);

  AttemptRecord attempt_record;
  attempt_record.operation = OperationKind::Reserve;
  attempt_record.subject = request.id;
  attempt_record.intent_digest = digest;
  attempt_record.revision = new_revision;
  attempt_record.epoch = target.epoch_;
  attempt_record.generation = ReservationGeneration::first();
  attempt_record.at_tick = request.now;
  FCR_TRYV(target.remember_attempt(request.attempt, attempt_record));

  outcome.reservation = target.view_of(target.records_.find(request.id)->second);
  outcome.revision = new_revision;
  outcome.epoch = target.epoch_;
  outcome.replayed = false;
  return ok();
}

// ---------------------------------------------------------------------------
// amend
// ---------------------------------------------------------------------------

Result<void> ReservationLedger::stage_amend(const AmendRequest& request, ReservationLedger& target,
                                            AmendOutcome& outcome) {
  const RequestLimits limits = effective_limits(target.options_.limits, request.limits);

  FCR_TRYV(require_ref(request.id.value(), "reservation id"));
  FCR_TRYV(require_attempt(request.attempt));
  FCR_TRYV(require_ref(request.actor.value(), "actor reference"));
  FCR_TRYV(request.validity.validate());
  FCR_TRYV(validate_amendment_cause(request.cause));
  FCR_TRYV(detail::require_detail(request.detail, limits.max_detail_bytes, "amendment detail"));
  if (request.expected_generation.is_zero()) {
    return Error(ErrorCode::MissingField, "an amendment must carry the generation it expects to amend");
  }
  if (request.now.is_zero()) {
    return Error(ErrorCode::MissingField, "an amendment requires the logical tick it is made at");
  }
  if (request.priority.has_value()) {
    FCR_TRYV(require_ref(request.priority->value(), "priority reference"));
  }
  if (request.headroom.has_value()) {
    FCR_TRYV(validate_headroom(*request.headroom));
  }
  FCR_TRY(canonical, detail::canonical_claims(request.claims, limits));

  AmendRequest normalized = request;
  normalized.claims = canonical;
  const std::string digest = intent_digest(normalized);

  const AttemptRecord* recorded = nullptr;
  switch (target.classify_attempt(request.attempt, OperationKind::Amend, digest, recorded)) {
    case AttemptVerdict::Conflict:
      return Error(ErrorCode::AttemptConflict,
                   "this attempt identity was already used for a different request on this ledger")
          .with_subject(request.attempt.value());
    case AttemptVerdict::Replay: {
      const auto position = target.records_.find(request.id);
      if (position == target.records_.end()) {
        return Error(ErrorCode::AttemptNotReplayable, "the attempt is recorded but its subject is no longer held")
            .with_subject(request.attempt.value());
      }
      outcome.reservation = target.view_of(position->second);
      outcome.previous_generation = request.expected_generation;
      outcome.revision = target.revision_;
      outcome.epoch = target.epoch_;
      outcome.replayed = true;
      return ok();
    }
    case AttemptVerdict::New:
      break;
  }

  FCR_TRYV(check_authority(target.epoch_, target.revision_, request.authority));

  const auto position = target.records_.find(request.id);
  if (position == target.records_.end()) {
    return Error(ErrorCode::ReservationNotFound, "no reservation with this identity is held")
        .with_subject(request.id.value());
  }
  const ReservationRecord& current = position->second;

  if (!reservation_state_holds_capacity(current.state)) {
    return Error(ErrorCode::ReservationTerminal, "a reservation that has ended cannot be amended")
        .with_subject(request.id.value() + " state=" + std::string(reservation_state_token(current.state)));
  }
  if (current.generation != request.expected_generation) {
    return Error(ErrorCode::StaleReservationGeneration,
                 "the amendment was issued against a reservation generation that has moved on")
        .with_subject("expected=" + format_unsigned(request.expected_generation.value()) +
                      " current=" + format_unsigned(current.generation.value()));
  }
  if (!detail::same_identity_text(current.claimant, request.actor)) {
    return Error(ErrorCode::ClaimantMismatch, "only the claimant of a reservation may amend it")
        .with_subject(request.id.value());
  }

  FCR_TRYV(require_consumable_capacity(target, request.expected_source_generation));

  if (request.validity.elapsed_at(request.now)) {
    return Error(ErrorCode::DeadlineAlreadyPassed, "the amended validity interval has already elapsed")
        .with_subject(request.id.value());
  }

  const HeadroomClass headroom = request.headroom.value_or(current.headroom);
  const PriorityRef priority = request.priority.value_or(current.priority);
  if (detail::same_claims(canonical, current.claims) && same_validity(request.validity, current.validity) &&
      headroom == current.headroom && priority == current.priority) {
    return Error(ErrorCode::AmendmentNoChange, "the amendment would not change the reservation")
        .with_subject(request.id.value());
  }
  if (current.lineage.size() + 1 > limits.max_lineage_entries) {
    return Error(ErrorCode::LineageOverflow, "the reservation has reached its amendment lineage bound")
        .with_subject(request.id.value());
  }

  // The reservation's own claims are released before the new ones are priced, so
  // an amendment that reshapes a commitment never fails for want of capacity it
  // already holds.
  for (const ResourceClaim& claim : canonical) {
    const auto account = target.accounting_.find(claim.pool);
    if (account == target.accounting_.end()) {
      return Error(ErrorCode::PoolUnknown, "the claim names a capacity pool that the held evidence does not declare")
          .with_subject(claim.pool.to_string());
    }
    FCR_TRY(available, checked_add(account->second.free.units(), current.claimed_from(claim.pool)));
    if (claim.amount > available) {
      return Error(ErrorCode::InsufficientCapacity, "the pool does not have enough free capacity for this amendment")
          .with_subject(claim.pool.to_string() + " required=" + format_unsigned(claim.amount) +
                        " available=" + format_unsigned(available));
    }
  }

  FCR_TRY(new_revision, target.revision_.next());
  FCR_TRY(new_generation, current.generation.next());

  ReservationRecord amended = current;
  amended.generation = new_generation;
  amended.authority_epoch = target.epoch_;
  amended.revision = new_revision;
  amended.last_attempt = request.attempt;
  amended.priority = priority;
  amended.headroom = headroom;
  amended.claims = canonical;
  amended.source_snapshot = target.capacity_.ref;
  amended.source_generation = target.capacity_.source_generation;
  amended.validity = request.validity;
  amended.updated_at_tick = request.now;
  amended.termination.reset();

  AmendmentEntry entry;
  entry.generation = new_generation;
  entry.predecessor = current.generation;
  entry.attempt = request.attempt;
  entry.revision = new_revision;
  entry.epoch = target.epoch_;
  entry.at_tick = request.now;
  entry.cause = request.cause;
  entry.detail = request.detail;
  amended.lineage.push_back(std::move(entry));

  const ReservationGeneration previous = current.generation;
  target.records_.insert_or_assign(request.id, std::move(amended));
  FCR_TRYV(target.rebuild_accounting());
  target.revision_ = new_revision;
  target.observe_tick(request.now);

  AttemptRecord attempt_record;
  attempt_record.operation = OperationKind::Amend;
  attempt_record.subject = request.id;
  attempt_record.intent_digest = digest;
  attempt_record.revision = new_revision;
  attempt_record.epoch = target.epoch_;
  attempt_record.generation = new_generation;
  attempt_record.at_tick = request.now;
  FCR_TRYV(target.remember_attempt(request.attempt, attempt_record));

  outcome.reservation = target.view_of(target.records_.find(request.id)->second);
  outcome.previous_generation = previous;
  outcome.revision = new_revision;
  outcome.epoch = target.epoch_;
  outcome.replayed = false;
  return ok();
}

// ---------------------------------------------------------------------------
// release
// ---------------------------------------------------------------------------

Result<void> ReservationLedger::stage_release(const ReleaseRequest& request, ReservationLedger& target,
                                              ReleaseOutcome& outcome) {
  const RequestLimits limits = effective_limits(target.options_.limits, request.limits);

  FCR_TRYV(require_ref(request.id.value(), "reservation id"));
  FCR_TRYV(require_attempt(request.attempt));
  FCR_TRYV(require_ref(request.actor.value(), "actor reference"));
  FCR_TRYV(detail::require_detail(request.detail, limits.max_detail_bytes, "release detail"));
  if (request.expected_generation.is_zero()) {
    return Error(ErrorCode::MissingField, "a release must carry the generation it expects to release");
  }
  if (request.now.is_zero()) {
    return Error(ErrorCode::MissingField, "a release requires the logical tick it is made at");
  }

  const std::string digest = intent_digest(request);
  const AttemptRecord* recorded = nullptr;
  switch (target.classify_attempt(request.attempt, OperationKind::Release, digest, recorded)) {
    case AttemptVerdict::Conflict:
      return Error(ErrorCode::AttemptConflict,
                   "this attempt identity was already used for a different request on this ledger")
          .with_subject(request.attempt.value());
    case AttemptVerdict::Replay: {
      const auto position = target.records_.find(request.id);
      if (position == target.records_.end()) {
        return Error(ErrorCode::AttemptNotReplayable, "the attempt is recorded but its subject is no longer held")
            .with_subject(request.attempt.value());
      }
      outcome.reservation = target.view_of(position->second);
      outcome.revision = target.revision_;
      outcome.epoch = target.epoch_;
      outcome.replayed = true;
      return ok();
    }
    case AttemptVerdict::New:
      break;
  }

  FCR_TRYV(check_authority(target.epoch_, target.revision_, request.authority));

  const auto position = target.records_.find(request.id);
  if (position == target.records_.end()) {
    return Error(ErrorCode::ReservationNotFound, "no reservation with this identity is held")
        .with_subject(request.id.value());
  }
  const ReservationRecord& current = position->second;
  if (!reservation_state_holds_capacity(current.state)) {
    return Error(ErrorCode::ReservationTerminal, "a reservation that has ended cannot be released")
        .with_subject(request.id.value() + " state=" + std::string(reservation_state_token(current.state)));
  }
  if (current.generation != request.expected_generation) {
    return Error(ErrorCode::StaleReservationGeneration,
                 "the release was issued against a reservation generation that has moved on")
        .with_subject("expected=" + format_unsigned(request.expected_generation.value()) +
                      " current=" + format_unsigned(current.generation.value()));
  }
  if (!detail::same_identity_text(current.claimant, request.actor)) {
    return Error(ErrorCode::ClaimantMismatch, "only the claimant of a reservation may release it")
        .with_subject(request.id.value());
  }

  FCR_TRY(new_revision, target.revision_.next());

  ReservationRecord released = current;
  released.state = ReservationState::Released;
  released.revision = new_revision;
  released.last_attempt = request.attempt;
  released.updated_at_tick = request.now;

  TerminationProvenance provenance;
  provenance.cause = TransitionCause::ClaimantRequest;
  provenance.actor = request.actor;
  provenance.attempt = request.attempt;
  provenance.revision = new_revision;
  provenance.epoch = target.epoch_;
  provenance.at_tick = request.now;
  provenance.detail = request.detail;
  released.termination = std::move(provenance);

  outcome.released = released.claims;

  target.records_.insert_or_assign(request.id, std::move(released));
  FCR_TRYV(target.rebuild_accounting());
  target.revision_ = new_revision;
  target.observe_tick(request.now);

  AttemptRecord attempt_record;
  attempt_record.operation = OperationKind::Release;
  attempt_record.subject = request.id;
  attempt_record.intent_digest = digest;
  attempt_record.revision = new_revision;
  attempt_record.epoch = target.epoch_;
  attempt_record.generation = current.generation;
  attempt_record.at_tick = request.now;
  FCR_TRYV(target.remember_attempt(request.attempt, attempt_record));

  outcome.reservation = target.view_of(target.records_.find(request.id)->second);
  outcome.revision = new_revision;
  outcome.epoch = target.epoch_;
  outcome.replayed = false;
  return ok();
}

// ---------------------------------------------------------------------------
// revoke
// ---------------------------------------------------------------------------

Result<void> ReservationLedger::stage_revoke(const RevokeRequest& request, ReservationLedger& target,
                                             RevokeOutcome& outcome) {
  const RequestLimits limits = effective_limits(target.options_.limits, request.limits);

  FCR_TRYV(require_ref(request.id.value(), "reservation id"));
  FCR_TRYV(require_attempt(request.attempt));
  FCR_TRYV(require_ref(request.actor.value(), "authority actor reference"));
  FCR_TRYV(detail::require_detail(request.detail, limits.max_detail_bytes, "revocation detail"));
  FCR_TRYV(validate_transition_cause(request.cause));
  if (request.expected_generation.is_zero()) {
    return Error(ErrorCode::MissingField, "a revocation must carry the generation it expects to revoke");
  }
  if (request.now.is_zero()) {
    return Error(ErrorCode::MissingField, "a revocation requires the logical tick it is made at");
  }
  if (request.policy.has_value()) {
    FCR_TRYV(require_ref(request.policy->value(), "policy reference"));
  }

  const std::string digest = intent_digest(request);
  const AttemptRecord* recorded = nullptr;
  switch (target.classify_attempt(request.attempt, OperationKind::Revoke, digest, recorded)) {
    case AttemptVerdict::Conflict:
      return Error(ErrorCode::AttemptConflict,
                   "this attempt identity was already used for a different request on this ledger")
          .with_subject(request.attempt.value());
    case AttemptVerdict::Replay: {
      const auto position = target.records_.find(request.id);
      if (position == target.records_.end()) {
        return Error(ErrorCode::AttemptNotReplayable, "the attempt is recorded but its subject is no longer held")
            .with_subject(request.attempt.value());
      }
      outcome.reservation = target.view_of(position->second);
      outcome.revision = target.revision_;
      outcome.epoch = target.epoch_;
      outcome.replayed = true;
      return ok();
    }
    case AttemptVerdict::New:
      break;
  }

  FCR_TRYV(check_authority(target.epoch_, target.revision_, request.authority));

  const auto position = target.records_.find(request.id);
  if (position == target.records_.end()) {
    return Error(ErrorCode::ReservationNotFound, "no reservation with this identity is held")
        .with_subject(request.id.value());
  }
  const ReservationRecord& current = position->second;
  if (!reservation_state_holds_capacity(current.state)) {
    return Error(ErrorCode::ReservationTerminal, "a reservation that has ended cannot be revoked")
        .with_subject(request.id.value() + " state=" + std::string(reservation_state_token(current.state)));
  }
  if (current.generation != request.expected_generation) {
    return Error(ErrorCode::StaleReservationGeneration,
                 "the revocation was issued against a reservation generation that has moved on")
        .with_subject("expected=" + format_unsigned(request.expected_generation.value()) +
                      " current=" + format_unsigned(current.generation.value()));
  }
  if (!detail::revocation_cause_allowed(request.cause)) {
    return Error(ErrorCode::InvalidArgument, "the requested transition cause cannot be used for a revocation")
        .with_subject(std::string(transition_cause_token(request.cause)));
  }

  // Authority to reclaim capacity is not authority to reclaim every class of
  // commitment: every revocation cites a policy, and a guaranteed commitment
  // additionally requires an explicit acknowledgement.
  if (!request.policy.has_value()) {
    return Error(ErrorCode::PolicyOverrideRequired, "a revocation must cite the policy that authorises it")
        .with_subject(request.id.value());
  }
  if (current.headroom == HeadroomClass::Guaranteed && !request.allow_guaranteed_override) {
    return Error(ErrorCode::HeadroomNotRevocable,
                 "a guaranteed commitment may only be revoked with an explicit override acknowledgement")
        .with_subject(request.id.value());
  }

  FCR_TRY(new_revision, target.revision_.next());

  ReservationRecord revoked = current;
  revoked.state = ReservationState::Revoked;
  revoked.revision = new_revision;
  revoked.last_attempt = request.attempt;
  revoked.updated_at_tick = request.now;

  TerminationProvenance provenance;
  provenance.cause = request.cause;
  provenance.actor = request.actor;
  provenance.attempt = request.attempt;
  provenance.revision = new_revision;
  provenance.epoch = target.epoch_;
  provenance.at_tick = request.now;
  provenance.policy = request.policy;
  provenance.detail = request.detail;
  revoked.termination = std::move(provenance);

  outcome.reclaimed = revoked.claims;

  target.records_.insert_or_assign(request.id, std::move(revoked));
  FCR_TRYV(target.rebuild_accounting());
  target.revision_ = new_revision;
  target.observe_tick(request.now);

  AttemptRecord attempt_record;
  attempt_record.operation = OperationKind::Revoke;
  attempt_record.subject = request.id;
  attempt_record.intent_digest = digest;
  attempt_record.revision = new_revision;
  attempt_record.epoch = target.epoch_;
  attempt_record.generation = current.generation;
  attempt_record.at_tick = request.now;
  FCR_TRYV(target.remember_attempt(request.attempt, attempt_record));

  outcome.reservation = target.view_of(target.records_.find(request.id)->second);
  outcome.revision = new_revision;
  outcome.epoch = target.epoch_;
  outcome.replayed = false;
  return ok();
}

// ---------------------------------------------------------------------------
// expire
// ---------------------------------------------------------------------------

Result<void> ReservationLedger::stage_expire(const ExpireRequest& request, ReservationLedger& target,
                                             ExpireOutcome& outcome) {
  const RequestLimits limits = effective_limits(target.options_.limits, request.limits);

  FCR_TRYV(require_attempt(request.attempt));
  if (request.now.is_zero()) {
    return Error(ErrorCode::MissingField, "an expiry sweep requires the logical tick it sweeps at");
  }

  const std::string digest = intent_digest(request);
  const AttemptRecord* recorded = nullptr;
  switch (target.classify_attempt(request.attempt, OperationKind::Expire, digest, recorded)) {
    case AttemptVerdict::Conflict:
      // A sweep carries no subject: reusing its identity with a different tick is
      // a different sweep, and the caller's intent cannot be inferred.
      return Error(ErrorCode::AttemptConflict,
                   "this attempt identity was already used for a different sweep on this ledger")
          .with_subject(request.attempt.value());
    case AttemptVerdict::Replay: {
      for (const auto& entry : target.records_) {
        if (entry.second.state != ReservationState::Expired || !entry.second.termination.has_value()) {
          continue;
        }
        if (entry.second.termination->attempt != request.attempt) {
          continue;
        }
        ExpiredReservation expired;
        expired.id = entry.first;
        expired.generation = entry.second.generation;
        expired.reclaimed = entry.second.claims;
        outcome.expired.push_back(std::move(expired));
      }
      outcome.revision = target.revision_;
      outcome.epoch = target.epoch_;
      outcome.replayed = true;
      return ok();
    }
    case AttemptVerdict::New:
      break;
  }

  FCR_TRYV(check_authority(target.epoch_, target.revision_, request.authority));

  std::vector<ReservationId> due;
  for (const auto& entry : target.records_) {
    if (reservation_state_holds_capacity(entry.second.state) && entry.second.validity.elapsed_at(request.now)) {
      due.push_back(entry.first);
    }
  }
  if (due.empty()) {
    // Nothing to do: the ledger is untouched, no revision is published and no
    // attempt identity is consumed.
    outcome.revision = target.revision_;
    outcome.epoch = target.epoch_;
    outcome.replayed = false;
    return ok();
  }
  if (due.size() > limits.max_expirations_per_sweep) {
    return Error(ErrorCode::LimitExceeded,
                 "more reservations are due than one sweep may end; raise the sweep bound or sweep sooner")
        .with_subject("due=" + format_unsigned(static_cast<std::uint64_t>(due.size())));
  }

  FCR_TRY(new_revision, target.revision_.next());

  for (const ReservationId& id : due) {
    ReservationRecord& record = target.records_.find(id)->second;
    record.state = ReservationState::Expired;
    record.revision = new_revision;
    record.last_attempt = request.attempt;
    record.updated_at_tick = request.now;

    TerminationProvenance provenance;
    provenance.cause = TransitionCause::DeadlineElapsed;
    provenance.attempt = request.attempt;
    provenance.revision = new_revision;
    provenance.epoch = target.epoch_;
    provenance.at_tick = request.now;
    provenance.detail = "the validity deadline elapsed";
    record.termination = std::move(provenance);

    ExpiredReservation expired;
    expired.id = id;
    expired.generation = record.generation;
    expired.reclaimed = record.claims;
    outcome.expired.push_back(std::move(expired));
  }

  FCR_TRYV(target.rebuild_accounting());
  target.revision_ = new_revision;
  target.observe_tick(request.now);

  AttemptRecord attempt_record;
  attempt_record.operation = OperationKind::Expire;
  attempt_record.intent_digest = digest;
  attempt_record.revision = new_revision;
  attempt_record.epoch = target.epoch_;
  attempt_record.at_tick = request.now;
  FCR_TRYV(target.remember_attempt(request.attempt, attempt_record));

  outcome.revision = new_revision;
  outcome.epoch = target.epoch_;
  outcome.replayed = false;
  return ok();
}

// ---------------------------------------------------------------------------
// reconcile
// ---------------------------------------------------------------------------

Result<void> ReservationLedger::stage_reconcile(const ReconcileRequest& request, ReservationLedger& target,
                                                ReconcileOutcome& outcome) {
  FCR_TRYV(require_attempt(request.attempt));
  FCR_TRYV(require_ref(request.actor.value(), "reconciliation actor reference"));
  FCR_TRYV(validate_reconcile_mode(request.mode));
  FCR_TRYV(detail::validate_snapshot_shape(request.snapshot, target.options_.max_pools));
  if (request.now.is_zero()) {
    return Error(ErrorCode::MissingField, "a reconciliation requires the logical tick it is made at");
  }
  if (request.policy.has_value()) {
    FCR_TRYV(require_ref(request.policy->value(), "policy reference"));
  }

  const std::string digest = intent_digest(request);
  const AttemptRecord* recorded = nullptr;
  switch (target.classify_attempt(request.attempt, OperationKind::Reconcile, digest, recorded)) {
    case AttemptVerdict::Conflict:
      return Error(ErrorCode::AttemptConflict,
                   "this attempt identity was already used for a different request on this ledger")
          .with_subject(request.attempt.value());
    case AttemptVerdict::Replay: {
      const SourceGeneration previous = recorded->previous_source_generation;
      if (target.capacity_.source_generation != request.snapshot.source_generation &&
          target.capacity_.source_generation != previous) {
        return Error(ErrorCode::AttemptNotReplayable,
                     "the attempt is recorded but the ledger no longer holds the capacity it adopted")
            .with_subject(request.attempt.value());
      }
      for (const auto& entry : target.records_) {
        if (!entry.second.termination.has_value() || entry.second.termination->attempt != request.attempt) {
          continue;
        }
        FencedReservation fenced;
        fenced.id = entry.first;
        fenced.generation = entry.second.generation;
        fenced.headroom = entry.second.headroom;
        fenced.cause = entry.second.termination->cause;
        outcome.fenced.push_back(std::move(fenced));
      }
      outcome.revision = target.revision_;
      outcome.epoch = target.epoch_;
      outcome.previous_source_generation = previous;
      outcome.source_generation = target.capacity_.source_generation;
      outcome.adopted = target.capacity_.source_generation == request.snapshot.source_generation;
      outcome.replayed = true;
      return ok();
    }
    case AttemptVerdict::New:
      break;
  }

  FCR_TRYV(check_authority(target.epoch_, target.revision_, request.authority));

  if (!target.has_capacity_) {
    return Error(ErrorCode::NoCapacityInstalled,
                 "no capacity evidence is installed; consume an initial snapshot before reconciling");
  }
  if (request.expected_source_generation.is_zero()) {
    return Error(ErrorCode::MissingField, "a reconciliation must carry the source generation it expects to replace");
  }
  if (request.expected_source_generation != target.capacity_.source_generation) {
    return Error(ErrorCode::SourceGenerationStale,
                 "the reconciliation was issued against a source generation that is no longer held")
        .with_subject("expected=" + format_unsigned(request.expected_source_generation.value()) +
                      " current=" + format_unsigned(target.capacity_.source_generation.value()));
  }
  if (request.snapshot.source_generation <= target.capacity_.source_generation) {
    return Error(ErrorCode::SourceGenerationConflict,
                 "the offered capacity snapshot is not newer than the evidence already held")
        .with_subject("offered=" + format_unsigned(request.snapshot.source_generation.value()) +
                      " held=" + format_unsigned(target.capacity_.source_generation.value()));
  }

  outcome.previous_source_generation = target.capacity_.source_generation;
  outcome.source_generation = request.snapshot.source_generation;
  outcome.revision = target.revision_;
  outcome.epoch = target.epoch_;

  std::map<PoolKey, std::pair<Units, Units>> held;
  FCR_TRYV(held_by_pool(target.records_, held));
  overcommits_against(held, request.snapshot, outcome.overcommits);

  if (outcome.overcommits.empty()) {
    target.capacity_ = request.snapshot;
    target.has_capacity_ = true;
    target.capacity_origin_ = CapacityOrigin::Consumed;
    FCR_TRYV(target.rebuild_accounting());
    FCR_TRY(new_revision, target.revision_.next());
    target.revision_ = new_revision;
    target.observe_tick(request.now);
    outcome.adopted = true;
    outcome.revision = new_revision;

    AttemptRecord attempt_record;
    attempt_record.operation = OperationKind::Reconcile;
    attempt_record.intent_digest = digest;
    attempt_record.revision = new_revision;
    attempt_record.epoch = target.epoch_;
    attempt_record.previous_source_generation = outcome.previous_source_generation;
    attempt_record.at_tick = request.now;
    FCR_TRYV(target.remember_attempt(request.attempt, attempt_record));
    return ok();
  }

  if (request.mode == ReconcileMode::Observe) {
    // Observation publishes nothing and consumes no attempt identity.
    outcome.adopted = false;
    outcome.fenced.clear();
    return ok();
  }

  if (!request.policy.has_value()) {
    return Error(ErrorCode::PolicyOverrideRequired,
                 "fencing commitments against withdrawn capacity requires a policy reference");
  }

  struct Candidate {
    ReservationId id;
    std::uint8_t rank = 0;
    Tick deadline;
  };
  std::vector<Candidate> candidates;
  for (const auto& entry : target.records_) {
    const ReservationRecord& record = entry.second;
    if (!reservation_state_holds_capacity(record.state) || record.headroom == HeadroomClass::Guaranteed) {
      continue;
    }
    bool touches = false;
    for (const PoolOvercommit& overcommit : outcome.overcommits) {
      if (record.claimed_from(overcommit.pool) != 0) {
        touches = true;
        break;
      }
    }
    if (!touches) {
      continue;
    }
    Candidate candidate;
    candidate.id = entry.first;
    candidate.rank = detail::headroom_fence_rank(record.headroom);
    candidate.deadline = record.validity.deadline;
    candidates.push_back(candidate);
  }
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
    if (lhs.rank != rhs.rank) {
      return lhs.rank < rhs.rank;
    }
    if (lhs.deadline != rhs.deadline) {
      return lhs.deadline < rhs.deadline;
    }
    return lhs.id < rhs.id;
  });

  FCR_TRY(new_revision, target.revision_.next());

  for (const Candidate& candidate : candidates) {
    FCR_TRYV(held_by_pool(target.records_, held));
    std::vector<PoolOvercommit> remaining;
    overcommits_against(held, request.snapshot, remaining);
    if (remaining.empty()) {
      break;
    }
    const ReservationRecord& record = target.records_.find(candidate.id)->second;
    bool touches = false;
    for (const PoolOvercommit& overcommit : remaining) {
      if (record.claimed_from(overcommit.pool) != 0) {
        touches = true;
        break;
      }
    }
    if (!touches) {
      continue;
    }

    ReservationRecord fenced = record;
    fenced.state = ReservationState::Revoked;
    fenced.revision = new_revision;
    fenced.last_attempt = request.attempt;
    fenced.updated_at_tick = request.now;

    TerminationProvenance provenance;
    provenance.cause = TransitionCause::CapacityWithdrawn;
    provenance.actor = request.actor;
    provenance.attempt = request.attempt;
    provenance.revision = new_revision;
    provenance.epoch = target.epoch_;
    provenance.at_tick = request.now;
    provenance.policy = request.policy;
    provenance.detail = "capacity was withdrawn by reconciliation to source generation " +
                        format_unsigned(request.snapshot.source_generation.value());
    fenced.termination = std::move(provenance);

    FencedReservation reported;
    reported.id = candidate.id;
    reported.generation = fenced.generation;
    reported.headroom = fenced.headroom;
    reported.cause = TransitionCause::CapacityWithdrawn;
    outcome.fenced.push_back(std::move(reported));
    target.records_.insert_or_assign(candidate.id, std::move(fenced));
  }

  FCR_TRYV(held_by_pool(target.records_, held));
  std::vector<PoolOvercommit> remaining;
  overcommits_against(held, request.snapshot, remaining);
  if (!remaining.empty()) {
    // Guaranteed commitments still exceed the reduced pool. Nothing is adopted
    // and nothing is fenced: a reconciliation either closes the accounting or
    // changes nothing at all.
    return Error(ErrorCode::UnresolvableOvercommit,
                 "the reduced capacity cannot be reconciled without revoking a guaranteed commitment")
        .with_subject(remaining.front().pool.to_string() + " excess=" + format_unsigned(remaining.front().excess));
  }

  target.capacity_ = request.snapshot;
  target.has_capacity_ = true;
  target.capacity_origin_ = CapacityOrigin::Consumed;
  FCR_TRYV(target.rebuild_accounting());
  target.revision_ = new_revision;
  target.observe_tick(request.now);
  outcome.adopted = true;
  outcome.revision = new_revision;

  AttemptRecord attempt_record;
  attempt_record.operation = OperationKind::Reconcile;
  attempt_record.intent_digest = digest;
  attempt_record.revision = new_revision;
  attempt_record.epoch = target.epoch_;
  attempt_record.previous_source_generation = outcome.previous_source_generation;
  attempt_record.at_tick = request.now;
  FCR_TRYV(target.remember_attempt(request.attempt, attempt_record));
  return ok();
}

}  // namespace dccp::facility_capacity_reservation
