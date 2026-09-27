// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Helpers shared by the ledger, its staged operations and the canonical codec.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_DETAIL_LEDGER_INTERNAL_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_DETAIL_LEDGER_INTERNAL_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_capacity_reservation/capacity.hpp"
#include "dccp/facility_capacity_reservation/request.hpp"
#include "dccp/facility_capacity_reservation/reservation.hpp"
#include "dccp/facility_capacity_reservation/status.hpp"

namespace dccp::facility_capacity_reservation::detail {

/// Unambiguous, length-prefixed field framing used by the intent digests.
void append_field(std::string& material, std::string_view value);
void append_claims(std::string& material, const std::vector<ResourceClaim>& claims);
void append_validity(std::string& material, const ValidityInterval& validity);

/// Validates a claim set and returns it in canonical order. Duplicate pools are
/// refused rather than merged.
Result<std::vector<ResourceClaim>> canonical_claims(const std::vector<ResourceClaim>& claims,
                                                    const RequestLimits& limits);

/// Checks that a snapshot is internally consistent, canonically ordered, unique
/// by pool and within the pool bound.
Result<void> validate_snapshot_shape(const CapacitySnapshot& snapshot, std::size_t max_pools);

/// Requires a request to carry a non-zero writer authority epoch.
Result<void> require_epoch(AuthorityEpoch epoch);

/// Requires free text to be within its bound.
Result<void> require_detail(std::string_view detail, std::size_t max_bytes, std::string_view what);

/// True when the two claim sets are identical, including order.
bool same_claims(const std::vector<ResourceClaim>& lhs, const std::vector<ResourceClaim>& rhs) noexcept;

/// True when a transition cause may be used for a facility revocation.
bool revocation_cause_allowed(TransitionCause cause) noexcept;

/// Ordering rank used to fence commitments deterministically: opportunistic
/// first, then firm. Guaranteed commitments are never fenced automatically.
std::uint8_t headroom_fence_rank(HeadroomClass headroom) noexcept;

/// Compares two identities that belong to different families.
///
/// The library never converts between identity families implicitly: a claimant
/// reference is not an actor reference, and making one from the other would
/// defeat the type system. The one place the two are deliberately compared is the
/// check that a release or amendment was issued by the reservation's own
/// claimant, and that check is written out here so it appears exactly once.
template <class Left, class Right>
bool same_identity_text(const StrongId<Left>& lhs, const StrongId<Right>& rhs) noexcept {
  return lhs.value() == rhs.value();
}

}  // namespace dccp::facility_capacity_reservation::detail

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_DETAIL_LEDGER_INTERNAL_HPP
