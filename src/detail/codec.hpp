// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Canonical serialization of the authoritative ledger state.
//
// The format is line oriented and fully framed: every collection declares its
// count, every record is delimited, every field is positional, and the document
// ends with an integrity digest over every preceding byte. The decoder is
// deliberately stricter than the encoder: it rejects unknown keys, reordered
// keys, wrong field counts, non-canonical integers, duplicate identities,
// unsorted collections, a mismatched digest and any trailing byte.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_DETAIL_CODEC_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_DETAIL_CODEC_HPP

#include <cstddef>
#include <string>
#include <string_view>

#include "dccp/facility_capacity_reservation/ledger.hpp"
#include "dccp/facility_capacity_reservation/status.hpp"

namespace dccp::facility_capacity_reservation::detail {

/// Bounds applied while decoding a persisted state document.
struct StateLimits {
  std::size_t max_bytes = kMaxDocumentBytes;
  std::size_t max_lines = 4'000'000;
  std::size_t max_reservations = 1'000'000;
  std::size_t max_attempts = 1'000'000;
  std::size_t max_pools = 4096;
  std::size_t max_pools_per_reservation = 256;
  std::size_t max_lineage_per_reservation = 1024;
};

/// Header line of a state document.
inline constexpr std::string_view kStateSignature = "fcr-state";

/// Renders the whole authoritative state.
Result<std::string> encode_state(const ReservationLedger& ledger);

/// Rebuilds a ledger from a state document.
///
/// `expected_incarnation` must equal the incarnation the document declares: a
/// document belonging to a different store incarnation is refused rather than
/// adopted. `epoch` is the current writer authority, which lives in the head
/// pointer rather than in the state document, because the epoch changes on every
/// writer handover while the state does not.
Result<ReservationLedger> decode_state(std::string_view document, const Incarnation& expected_incarnation,
                                       AuthorityEpoch epoch, LedgerOptions options, CapacityOrigin origin,
                                       const StateLimits& limits);

/// True when `document` begins with the state signature, without validating the
/// rest. Used only to give a clearer error for a file that is not a state at all.
bool looks_like_state(std::string_view document) noexcept;

}  // namespace dccp::facility_capacity_reservation::detail

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_DETAIL_CODEC_HPP
