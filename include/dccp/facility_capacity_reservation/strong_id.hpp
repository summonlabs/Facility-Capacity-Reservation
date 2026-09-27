// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Strongly typed identities, counters, generations, epochs and revisions.
//
// Each family below is a distinct C++ type. There is deliberately no converting
// constructor between families and no implicit conversion to the underlying
// representation, so a tenant reference cannot be passed where a claimant
// reference is expected and a source generation cannot be passed where a
// reservation generation is expected.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_STRONG_ID_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_STRONG_ID_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "dccp/facility_capacity_reservation/status.hpp"
#include "dccp/facility_capacity_reservation/text.hpp"

namespace dccp::facility_capacity_reservation {

/// A validated, bounded identifier belonging to exactly one identity family.
///
/// An identifier is opaque to this library: it names an object owned by another
/// runtime (a tenant, a service, a facility, a capacity pool scope) or by this
/// one (a reservation, an attempt). The library never interprets its structure.
template <class Tag>
class StrongId {
 public:
  StrongId() = default;

  /// Builds an identifier from already-validated text. Prefer parse().
  static StrongId from_validated(std::string value) {
    StrongId id;
    id.value_ = std::move(value);
    return id;
  }

  /// Validates and builds an identifier. Rejects empty, over-long and
  /// non-canonical text with a stable code.
  static Result<StrongId> parse(std::string_view text, std::string_view what) {
    FCR_TRYV(validate_identifier(text, what));
    return StrongId::from_validated(std::string(text));
  }

  const std::string& value() const noexcept { return value_; }
  bool empty() const noexcept { return value_.empty(); }

  /// The identifier text, or "-" when absent, in canonical documents.
  std::string to_string() const { return value_.empty() ? std::string("-") : value_; }

  friend bool operator==(const StrongId& lhs, const StrongId& rhs) noexcept { return lhs.value_ == rhs.value_; }
  friend bool operator!=(const StrongId& lhs, const StrongId& rhs) noexcept { return !(lhs == rhs); }
  friend bool operator<(const StrongId& lhs, const StrongId& rhs) noexcept { return lhs.value_ < rhs.value_; }

 private:
  std::string value_;
};

struct ReservationIdTag;
struct ClaimantRefTag;
struct TenantRefTag;
struct ServiceRefTag;
struct PriorityRefTag;
struct PolicyRefTag;
struct EvidenceRefTag;
struct ActorRefTag;
struct ScopeRefTag;
struct FacilityRefTag;
struct SnapshotRefTag;
struct AttemptIdTag;

/// Identifier of a reservation. Stable across amendments: an amendment produces
/// a new generation of the same reservation identity.
using ReservationId = StrongId<ReservationIdTag>;

/// The party that holds the commitment.
using ClaimantRef = StrongId<ClaimantRefTag>;

/// The tenant the commitment is accounted to.
using TenantRef = StrongId<TenantRefTag>;

/// The service the commitment exists for.
using ServiceRef = StrongId<ServiceRefTag>;

/// Opaque reference to a priority defined by the priority authority.
using PriorityRef = StrongId<PriorityRefTag>;

/// Opaque reference to a policy that justified a privileged operation.
using PolicyRef = StrongId<PolicyRefTag>;

/// Opaque reference to evidence supporting a decision.
using EvidenceRef = StrongId<EvidenceRefTag>;

/// The actor that performed an operation. Compared against a record's claimant
/// to enforce that only the claimant may release or amend its own commitment.
using ActorRef = StrongId<ActorRefTag>;

/// Opaque scope of a capacity pool: a hall, a zone, a power domain, a cooling
/// domain or a facility service, as named by the capacity snapshot. This library
/// never derives or interprets the scope; it only matches it exactly.
using ScopeRef = StrongId<ScopeRefTag>;

/// Opaque reference to the facility the snapshot describes.
using FacilityRef = StrongId<FacilityRefTag>;

/// Opaque reference to the consumed capacity snapshot document.
using SnapshotRef = StrongId<SnapshotRefTag>;

/// Client-supplied identity of one attempted mutation.
///
/// An attempt identifier is the unit of idempotency: repeating a mutation with
/// the same attempt identity and the same request replays the recorded outcome
/// instead of acting twice.
using AttemptId = StrongId<AttemptIdTag>;

/// A monotonic, non-zero 64-bit counter belonging to exactly one family.
template <class Tag>
class StrongCounter {
 public:
  using value_type = std::uint64_t;

  StrongCounter() = default;
  explicit constexpr StrongCounter(std::uint64_t value) noexcept : value_(value) {}

  static constexpr StrongCounter first() noexcept { return StrongCounter(1); }

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == 0; }

  /// Next value, refusing to wrap. Exhaustion is reported, never wrapped.
  Result<StrongCounter> next() const {
    if (value_ == kMax) {
      return Error(ErrorCode::ArithmeticOverflow, std::string(Tag::overflow_code_name()) + " counter exhausted");
    }
    return StrongCounter(value_ + 1);
  }

  friend constexpr bool operator==(StrongCounter lhs, StrongCounter rhs) noexcept { return lhs.value_ == rhs.value_; }
  friend constexpr bool operator!=(StrongCounter lhs, StrongCounter rhs) noexcept { return !(lhs == rhs); }
  friend constexpr bool operator<(StrongCounter lhs, StrongCounter rhs) noexcept { return lhs.value_ < rhs.value_; }
  friend constexpr bool operator<=(StrongCounter lhs, StrongCounter rhs) noexcept { return lhs.value_ <= rhs.value_; }
  friend constexpr bool operator>(StrongCounter lhs, StrongCounter rhs) noexcept { return rhs < lhs; }
  friend constexpr bool operator>=(StrongCounter lhs, StrongCounter rhs) noexcept { return rhs <= lhs; }

  static constexpr std::uint64_t kMax = 0xFFFF'FFFF'FFFF'FFFFULL;

 private:
  std::uint64_t value_ = 0;
};

struct ReservationGenerationTag {
  static constexpr std::string_view overflow_code_name() noexcept { return "reservation generation"; }
};
struct AuthorityEpochTag {
  static constexpr std::string_view overflow_code_name() noexcept { return "authority epoch"; }
};
struct RevisionTag {
  static constexpr std::string_view overflow_code_name() noexcept { return "state revision"; }
};
struct SourceGenerationTag {
  static constexpr std::string_view overflow_code_name() noexcept { return "source generation"; }
};
struct TickTag {
  static constexpr std::string_view overflow_code_name() noexcept { return "logical tick"; }
};

/// Generation of one reservation identity. Generation 1 is the original
/// reservation; every successful amendment mints the next generation.
using ReservationGeneration = StrongCounter<ReservationGenerationTag>;

/// Writer authority epoch of a store incarnation.
///
/// Every time a process takes write authority over a store it advances the
/// epoch. Any operation carrying an older epoch is refused rather than merged,
/// so a writer that lost authority — through crash, restart or a second writer —
/// cannot publish a decision taken under authority it no longer holds.
using AuthorityEpoch = StrongCounter<AuthorityEpochTag>;

/// Monotonic revision of the published authoritative state. Every accepted
/// mutation advances it by exactly one.
using Revision = StrongCounter<RevisionTag>;

/// Generation of the consumed capacity snapshot, as declared by its producer.
using SourceGeneration = StrongCounter<SourceGenerationTag>;

/// Logical monotonic tick used for validity intervals and provenance.
using Tick = StrongCounter<TickTag>;

/// Identity of one durable store incarnation: a store directory retains its
/// incarnation for life, so a store that was replaced by a different one is
/// refused rather than silently adopted.
class Incarnation {
 public:
  Incarnation() = default;

  static Result<Incarnation> parse(std::string_view text) {
    if (!is_lower_hex(text, kHexDigits)) {
      return Error(ErrorCode::MalformedIdentifier, "incarnation must be 32 lowercase hexadecimal digits");
    }
    FCR_TRY(bytes, parse_hex_bytes(text, kBytes));
    Incarnation incarnation;
    incarnation.bytes_ = std::move(bytes);
    return incarnation;
  }

  /// Draws a fresh incarnation from operating-system entropy mixed with the
  /// process identity and a monotonic counter.
  ///
  /// The value distinguishes store incarnations; it is not a secret and is not
  /// used as a credential.
  static Incarnation generate();

  bool empty() const noexcept { return bytes_.empty(); }
  bool operator==(const Incarnation& other) const noexcept { return bytes_ == other.bytes_; }
  bool operator!=(const Incarnation& other) const noexcept { return !(*this == other); }

  std::string to_string() const { return format_hex(bytes_); }

  static constexpr std::size_t kBytes = 16;
  static constexpr std::size_t kHexDigits = kBytes * 2;

 private:
  std::vector<std::uint8_t> bytes_;
};

/// Version of the persisted canonical state format. A store written by a newer
/// format is refused rather than guessed at.
inline constexpr std::uint32_t kStateFormatVersion = 1;

/// Version of the consumed capacity snapshot document format.
inline constexpr std::uint32_t kSnapshotFormatVersion = 1;

}  // namespace dccp::facility_capacity_reservation

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_STRONG_ID_HPP
