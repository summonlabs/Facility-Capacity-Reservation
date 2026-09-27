// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "detail/ledger_internal.hpp"

#include <algorithm>

#include "dccp/facility_capacity_reservation/text.hpp"

namespace dccp::facility_capacity_reservation::detail {
namespace {

bool is_valid_kind(ResourceKind kind) noexcept {
  switch (kind) {
    case ResourceKind::Space:
    case ResourceKind::Rack:
    case ResourceKind::Power:
    case ResourceKind::Cooling:
    case ResourceKind::FacilityService:
      return true;
  }
  return false;
}

}  // namespace

void append_field(std::string& material, std::string_view value) {
  material.append(format_unsigned(static_cast<std::uint64_t>(value.size())));
  material.push_back(':');
  material.append(value);
  material.push_back(';');
}

void append_claims(std::string& material, const std::vector<ResourceClaim>& claims) {
  append_field(material, format_unsigned(static_cast<std::uint64_t>(claims.size())));
  for (const ResourceClaim& claim : claims) {
    append_field(material, resource_kind_token(claim.pool.kind));
    append_field(material, claim.pool.scope.value());
    append_field(material, format_unsigned(claim.amount));
  }
}

void append_validity(std::string& material, const ValidityInterval& validity) {
  append_field(material, format_unsigned(validity.start.value()));
  append_field(material, format_unsigned(validity.deadline.value()));
}

Result<std::vector<ResourceClaim>> canonical_claims(const std::vector<ResourceClaim>& claims,
                                                    const RequestLimits& limits) {
  if (claims.empty()) {
    return Error(ErrorCode::ClaimEmpty, "a reservation must claim at least one capacity pool");
  }
  if (claims.size() > limits.max_claims) {
    return Error(ErrorCode::LimitExceeded, "the claim set is larger than the configured maximum");
  }
  std::vector<ResourceClaim> canonical = claims;
  for (const ResourceClaim& claim : canonical) {
    if (!is_valid_kind(claim.pool.kind)) {
      return Error(ErrorCode::InvalidArgument, "the claim names an unknown resource kind");
    }
    if (claim.pool.scope.empty()) {
      return Error(ErrorCode::MissingField, "every claim must name a capacity pool scope");
    }
    FCR_TRYV(validate_identifier(claim.pool.scope.value(), "claim scope"));
    if (claim.amount == 0) {
      return Error(ErrorCode::ClaimAmountZero, "every claim must ask for at least one unit")
          .with_subject(claim.pool.to_string());
    }
  }
  std::sort(canonical.begin(), canonical.end());
  for (std::size_t index = 1; index < canonical.size(); ++index) {
    if (canonical[index].pool == canonical[index - 1].pool) {
      return Error(ErrorCode::ClaimDuplicatePool, "a claim set must name each capacity pool at most once")
          .with_subject(canonical[index].pool.to_string());
    }
  }
  return canonical;
}

Result<void> validate_snapshot_shape(const CapacitySnapshot& snapshot, std::size_t max_pools) {
  if (snapshot.pools.empty()) {
    return Error(ErrorCode::SnapshotEmpty, "a capacity snapshot must declare at least one pool");
  }
  if (snapshot.pools.size() > max_pools) {
    return Error(ErrorCode::LimitExceeded, "the capacity snapshot declares more pools than the configured maximum");
  }
  if (snapshot.ref.empty()) {
    return Error(ErrorCode::MissingField, "a capacity snapshot must name the document it was consumed from");
  }
  FCR_TRYV(validate_identifier(snapshot.ref.value(), "snapshot ref"));
  if (snapshot.facility.empty()) {
    return Error(ErrorCode::MissingField, "a capacity snapshot must name the facility it describes");
  }
  FCR_TRYV(validate_identifier(snapshot.facility.value(), "facility ref"));
  if (snapshot.source_generation.is_zero()) {
    return Error(ErrorCode::MissingField, "a capacity snapshot must declare a non-zero source generation");
  }
  for (std::size_t index = 0; index < snapshot.pools.size(); ++index) {
    const CapacityPool& pool = snapshot.pools[index];
    if (pool.key.scope.empty()) {
      return Error(ErrorCode::MissingField, "every capacity pool must name its scope");
    }
    FCR_TRYV(validate_identifier(pool.key.scope.value(), "pool scope"));
    if (!is_valid_kind(pool.key.kind)) {
      return Error(ErrorCode::InvalidArgument, "a capacity pool names an unknown resource kind")
          .with_subject(pool.key.to_string());
    }
    if (index > 0 && !(snapshot.pools[index - 1].key < pool.key)) {
      return Error(ErrorCode::NonCanonicalOrder, "capacity pools must be unique and in canonical order")
          .with_subject(pool.key.to_string());
    }
    FCR_TRYV(pool.available());
    FCR_TRYV(pool.reservable());
  }
  return ok();
}

Result<void> require_epoch(AuthorityEpoch epoch) {
  if (epoch.is_zero()) {
    return Error(ErrorCode::AuthorityRequired, "the request must carry the writer authority epoch it acts under");
  }
  return ok();
}

Result<void> require_detail(std::string_view detail, std::size_t max_bytes, std::string_view what) {
  if (detail.size() > max_bytes) {
    return Error(ErrorCode::TextTooLong, std::string(what) + " is longer than the configured maximum");
  }
  return ok();
}

bool same_claims(const std::vector<ResourceClaim>& lhs, const std::vector<ResourceClaim>& rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    if (lhs[index] != rhs[index]) {
      return false;
    }
  }
  return true;
}

bool revocation_cause_allowed(TransitionCause cause) noexcept {
  switch (cause) {
    case TransitionCause::AuthorityRevocation:
    case TransitionCause::CapacityWithdrawn:
    case TransitionCause::FacilityOverride:
      return true;
    case TransitionCause::ClaimantRequest:
    case TransitionCause::DeadlineElapsed:
    case TransitionCause::Superseded:
      return false;
  }
  return false;
}

std::uint8_t headroom_fence_rank(HeadroomClass headroom) noexcept {
  switch (headroom) {
    case HeadroomClass::Opportunistic:
      return 0;
    case HeadroomClass::Firm:
      return 1;
    case HeadroomClass::Guaranteed:
      return 2;
  }
  return 3;
}

}  // namespace dccp::facility_capacity_reservation::detail
