// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity_reservation/reservation.hpp"

#include <algorithm>

#include "dccp/facility_capacity_reservation/request.hpp"

namespace dccp::facility_capacity_reservation {

std::string_view headroom_class_token(HeadroomClass value) noexcept {
  switch (value) {
    case HeadroomClass::Guaranteed:
      return "guaranteed";
    case HeadroomClass::Firm:
      return "firm";
    case HeadroomClass::Opportunistic:
      return "opportunistic";
  }
  return "unknown";
}

Result<HeadroomClass> parse_headroom_class(std::string_view token) {
  if (token == "guaranteed") {
    return HeadroomClass::Guaranteed;
  }
  if (token == "firm") {
    return HeadroomClass::Firm;
  }
  if (token == "opportunistic") {
    return HeadroomClass::Opportunistic;
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown headroom class token").with_subject(sanitize_for_display(token));
}

std::string_view headroom_class_name(HeadroomClass value) noexcept {
  switch (value) {
    case HeadroomClass::Guaranteed:
      return "guaranteed";
    case HeadroomClass::Firm:
      return "firm";
    case HeadroomClass::Opportunistic:
      return "opportunistic";
  }
  return "unknown";
}

std::string_view reservation_state_token(ReservationState value) noexcept {
  switch (value) {
    case ReservationState::Active:
      return "active";
    case ReservationState::Released:
      return "released";
    case ReservationState::Expired:
      return "expired";
    case ReservationState::Revoked:
      return "revoked";
  }
  return "unknown";
}

Result<ReservationState> parse_reservation_state(std::string_view token) {
  if (token == "active") {
    return ReservationState::Active;
  }
  if (token == "released") {
    return ReservationState::Released;
  }
  if (token == "expired") {
    return ReservationState::Expired;
  }
  if (token == "revoked") {
    return ReservationState::Revoked;
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown reservation state token").with_subject(sanitize_for_display(token));
}

std::string_view reservation_state_name(ReservationState value) noexcept {
  switch (value) {
    case ReservationState::Active:
      return "active";
    case ReservationState::Released:
      return "released";
    case ReservationState::Expired:
      return "expired";
    case ReservationState::Revoked:
      return "revoked";
  }
  return "unknown";
}

std::string_view transition_cause_token(TransitionCause value) noexcept {
  switch (value) {
    case TransitionCause::ClaimantRequest:
      return "claimant-request";
    case TransitionCause::DeadlineElapsed:
      return "deadline-elapsed";
    case TransitionCause::AuthorityRevocation:
      return "authority-revocation";
    case TransitionCause::CapacityWithdrawn:
      return "capacity-withdrawn";
    case TransitionCause::Superseded:
      return "superseded";
    case TransitionCause::FacilityOverride:
      return "facility-override";
  }
  return "unknown";
}

Result<TransitionCause> parse_transition_cause(std::string_view token) {
  if (token == "claimant-request") {
    return TransitionCause::ClaimantRequest;
  }
  if (token == "deadline-elapsed") {
    return TransitionCause::DeadlineElapsed;
  }
  if (token == "authority-revocation") {
    return TransitionCause::AuthorityRevocation;
  }
  if (token == "capacity-withdrawn") {
    return TransitionCause::CapacityWithdrawn;
  }
  if (token == "superseded") {
    return TransitionCause::Superseded;
  }
  if (token == "facility-override") {
    return TransitionCause::FacilityOverride;
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown transition cause token").with_subject(sanitize_for_display(token));
}

std::string_view transition_cause_name(TransitionCause value) noexcept {
  switch (value) {
    case TransitionCause::ClaimantRequest:
      return "claimant request";
    case TransitionCause::DeadlineElapsed:
      return "deadline elapsed";
    case TransitionCause::AuthorityRevocation:
      return "authority revocation";
    case TransitionCause::CapacityWithdrawn:
      return "capacity withdrawn";
    case TransitionCause::Superseded:
      return "superseded by amendment";
    case TransitionCause::FacilityOverride:
      return "facility policy override";
  }
  return "unknown";
}

std::string_view amendment_cause_token(AmendmentCause value) noexcept {
  switch (value) {
    case AmendmentCause::Correction:
      return "correction";
    case AmendmentCause::CapacityIncrease:
      return "capacity-increase";
    case AmendmentCause::CapacityDecrease:
      return "capacity-decrease";
    case AmendmentCause::ScopeChange:
      return "scope-change";
    case AmendmentCause::DeadlineExtension:
      return "deadline-extension";
    case AmendmentCause::DeadlineReduction:
      return "deadline-reduction";
    case AmendmentCause::PriorityChange:
      return "priority-change";
    case AmendmentCause::HeadroomChange:
      return "headroom-change";
    case AmendmentCause::Other:
      return "other";
  }
  return "unknown";
}

Result<AmendmentCause> parse_amendment_cause(std::string_view token) {
  if (token == "correction") {
    return AmendmentCause::Correction;
  }
  if (token == "capacity-increase") {
    return AmendmentCause::CapacityIncrease;
  }
  if (token == "capacity-decrease") {
    return AmendmentCause::CapacityDecrease;
  }
  if (token == "scope-change") {
    return AmendmentCause::ScopeChange;
  }
  if (token == "deadline-extension") {
    return AmendmentCause::DeadlineExtension;
  }
  if (token == "deadline-reduction") {
    return AmendmentCause::DeadlineReduction;
  }
  if (token == "priority-change") {
    return AmendmentCause::PriorityChange;
  }
  if (token == "headroom-change") {
    return AmendmentCause::HeadroomChange;
  }
  if (token == "other") {
    return AmendmentCause::Other;
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown amendment cause token").with_subject(sanitize_for_display(token));
}

std::string_view amendment_cause_name(AmendmentCause value) noexcept {
  switch (value) {
    case AmendmentCause::Correction:
      return "correction";
    case AmendmentCause::CapacityIncrease:
      return "capacity increase";
    case AmendmentCause::CapacityDecrease:
      return "capacity decrease";
    case AmendmentCause::ScopeChange:
      return "scope change";
    case AmendmentCause::DeadlineExtension:
      return "deadline extension";
    case AmendmentCause::DeadlineReduction:
      return "deadline reduction";
    case AmendmentCause::PriorityChange:
      return "priority change";
    case AmendmentCause::HeadroomChange:
      return "headroom change";
    case AmendmentCause::Other:
      return "other";
  }
  return "unknown";
}

std::string_view revalidation_class_token(RevalidationClass value) noexcept {
  switch (value) {
    case RevalidationClass::Current:
      return "current";
    case RevalidationClass::DeadlinePassed:
      return "deadline-passed";
    case RevalidationClass::SourceGenerationStale:
      return "source-generation-stale";
    case RevalidationClass::PoolMissing:
      return "pool-missing";
    case RevalidationClass::CapacityExceeded:
      return "capacity-exceeded";
  }
  return "unknown";
}

Result<RevalidationClass> parse_revalidation_class(std::string_view token) {
  if (token == "current") {
    return RevalidationClass::Current;
  }
  if (token == "deadline-passed") {
    return RevalidationClass::DeadlinePassed;
  }
  if (token == "source-generation-stale") {
    return RevalidationClass::SourceGenerationStale;
  }
  if (token == "pool-missing") {
    return RevalidationClass::PoolMissing;
  }
  if (token == "capacity-exceeded") {
    return RevalidationClass::CapacityExceeded;
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown revalidation class token")
      .with_subject(sanitize_for_display(token));
}

std::string_view reconcile_mode_token(ReconcileMode value) noexcept {
  switch (value) {
    case ReconcileMode::Observe:
      return "observe";
    case ReconcileMode::Enforce:
      return "enforce";
  }
  return "unknown";
}

Result<ReconcileMode> parse_reconcile_mode(std::string_view token) {
  if (token == "observe") {
    return ReconcileMode::Observe;
  }
  if (token == "enforce") {
    return ReconcileMode::Enforce;
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown reconcile mode token").with_subject(sanitize_for_display(token));
}

Result<Units> ReservationRecord::claimed_of_kind(ResourceKind kind) const {
  Units total = 0;
  for (const ResourceClaim& claim : claims) {
    if (claim.pool.kind != kind) {
      continue;
    }
    FCR_TRYV(checked_accumulate(total, claim.amount));
  }
  return total;
}

Units ReservationRecord::claimed_from(const PoolKey& key) const {
  const auto position =
      std::lower_bound(claims.begin(), claims.end(), key,
                       [](const ResourceClaim& claim, const PoolKey& wanted) { return claim.pool < wanted; });
  if (position == claims.end() || position->pool != key) {
    return 0;
  }
  return position->amount;
}

}  // namespace dccp::facility_capacity_reservation
