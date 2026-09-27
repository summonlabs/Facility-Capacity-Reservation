// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Facility capacity as consumed from a capacity snapshot.
//
// This library does not derive facility capacity. It consumes a snapshot that
// was produced and authority-signed by the facility capacity runtimes, records
// the exact source generation it came from, and refuses to act on capacity it
// cannot attribute to a specific generation.
//
// Capacity is expressed as a set of pools. A pool is identified by the pair
// (resource kind, scope reference). The scope reference is opaque: this library
// never decides which hall, zone, power domain or service a commitment belongs
// to — the caller and the snapshot do.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_CAPACITY_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_CAPACITY_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_capacity_reservation/strong_id.hpp"
#include "dccp/facility_capacity_reservation/status.hpp"
#include "dccp/facility_capacity_reservation/units.hpp"

namespace dccp::facility_capacity_reservation {

/// The kinds of facility capacity this library may commit.
///
/// The enumeration is closed and deliberately contains no accelerator compute,
/// accelerator memory or network bandwidth kind: those are ASI and DFI
/// authorities. Numbers are part of the persisted format and are never reused.
enum class ResourceKind : std::uint8_t {
  Space = 1,            // unit: one space slot of the scope (footprint allocation)
  Rack = 2,             // unit: one rack position within the scope
  Power = 3,            // unit: one watt of electrical capacity
  Cooling = 4,          // unit: one watt of heat rejection capacity
  FacilityService = 5,  // unit: one unit of an explicitly named facility service
};

/// Stable canonical token for a resource kind.
std::string_view resource_kind_token(ResourceKind kind) noexcept;

/// Parses a canonical resource kind token. Unknown tokens are refused; there is
/// no "other" kind and no silent fallback.
Result<ResourceKind> parse_resource_kind(std::string_view token);

/// Human-readable name of a resource kind, for diagnostics.
std::string_view resource_kind_name(ResourceKind kind) noexcept;

/// Unit label of a resource kind, for diagnostics and CLI output.
std::string_view resource_kind_unit(ResourceKind kind) noexcept;

/// Identifies one capacity pool: a resource kind within one opaque scope.
struct PoolKey {
  ResourceKind kind = ResourceKind::Space;
  ScopeRef scope;

  friend bool operator==(const PoolKey& lhs, const PoolKey& rhs) noexcept {
    return lhs.kind == rhs.kind && lhs.scope == rhs.scope;
  }
  friend bool operator!=(const PoolKey& lhs, const PoolKey& rhs) noexcept { return !(lhs == rhs); }
  friend bool operator<(const PoolKey& lhs, const PoolKey& rhs) noexcept {
    if (lhs.kind != rhs.kind) {
      return static_cast<std::uint8_t>(lhs.kind) < static_cast<std::uint8_t>(rhs.kind);
    }
    return lhs.scope < rhs.scope;
  }

  std::string to_string() const {
    return std::string(resource_kind_token(kind)) + ":" + scope.value();
  }
};

/// One capacity pool as declared by the source snapshot.
///
/// The three declared quantities are all supplied by the snapshot producer; none
/// is derived here.
///
///   gross      nominal capacity of the pool
///   withdrawn  capacity the facility has explicitly removed from service
///   floor      capacity reserved for facility-critical use and not reservable
///
/// available  = gross - withdrawn     (checked)
/// reservable = available - floor     (checked)
struct CapacityPool {
  PoolKey key;
  Units gross = 0;
  Units withdrawn = 0;
  Units floor = 0;

  /// gross - withdrawn.
  Result<Units> available() const {
    if (withdrawn > gross) {
      return Error(ErrorCode::SnapshotInconsistent,
                   "pool declares more withdrawn capacity than gross capacity")
          .with_subject(key.to_string());
    }
    return gross - withdrawn;
  }

  /// available - floor. This is the quantity that must equal
  /// committed + protected + free at every published revision.
  Result<Units> reservable() const {
    FCR_TRY(available_units, available());
    if (floor > available_units) {
      return Error(ErrorCode::SnapshotInconsistent, "pool declares a floor larger than its available capacity")
          .with_subject(key.to_string());
    }
    return available_units - floor;
  }
};

/// A consumed capacity snapshot.
struct CapacitySnapshot {
  /// Identity of the snapshot document this capacity was consumed from.
  SnapshotRef ref;

  /// Generation declared by the producer. Every operation that consumes capacity
  /// records the generation it acted on, and reconciliation carries an exact
  /// precondition on it.
  SourceGeneration source_generation;

  /// Opaque reference to the facility the snapshot describes.
  FacilityRef facility;

  /// Producer's capture tick, in the same logical tick domain as validity
  /// intervals. Monotonic within a producer; this library never compares it to
  /// its own clock.
  Tick captured_at_tick;

  /// Pools, in canonical (kind, scope) order, unique by key.
  std::vector<CapacityPool> pools;

  /// SHA-256 of the canonical snapshot body, as imported. Integrity evidence for
  /// the consumed capacity; retained so that a reconciliation can be attributed
  /// to a specific document.
  std::string body_digest;

  /// Looks up a pool by key.
  const CapacityPool* find(const PoolKey& key) const;

  /// Number of pools.
  std::size_t size() const noexcept { return pools.size(); }

  /// Total reservable units of one kind across every scope of that kind,
  /// checked. Used for reporting only: commitments are always per pool.
  Result<Units> reservable_of_kind(ResourceKind kind) const;
};

/// Bounds applied while importing a capacity snapshot. Untrusted documents are
/// rejected before allocation.
struct SnapshotLimits {
  std::size_t max_bytes = kMaxDocumentBytes;
  std::size_t max_pools = 4096;
  std::size_t max_lines = 200'000;
};

/// Parses a capacity snapshot from its canonical document form.
///
/// The document is untrusted input: version, framing, ordering, field set,
/// integer canonicality, identifier validity, cross-field consistency, declared
/// counts and the trailing integrity digest are all verified before the snapshot
/// is returned. Nothing is allocated from a declared count before the bound is
/// checked.
Result<CapacitySnapshot> parse_capacity_snapshot(std::string_view document, const SnapshotLimits& limits);

/// Renders a snapshot back to canonical document form. Used by the import tool
/// and by tests that prove round-trip stability.
Result<std::string> write_capacity_snapshot(const CapacitySnapshot& snapshot);

}  // namespace dccp::facility_capacity_reservation

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_CAPACITY_HPP
