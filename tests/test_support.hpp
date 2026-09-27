// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Test support: temporary directories, deterministic randomness, request
// builders, and an independent reference model of the capacity accounting.
//
// The reference model exists so that the library's accounting is never verified
// against itself. It is written from the definition (committed + protected +
// free == reservable, per pool) using a separate traversal and separate
// accumulation, and it never calls into the library's ledger.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_TESTS_TEST_SUPPORT_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_TESTS_TEST_SUPPORT_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "dccp/facility_capacity_reservation/capacity.hpp"
#include "dccp/facility_capacity_reservation/ledger.hpp"
#include "dccp/facility_capacity_reservation/request.hpp"
#include "dccp/facility_capacity_reservation/status.hpp"
#include "dccp/facility_capacity_reservation/store.hpp"
#include "test_framework.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace fcr_test {

/// A unique directory under the system temporary directory, removed on
/// destruction. Nothing outside it is ever touched.
class TempDir {
 public:
  explicit TempDir(const std::string& label);
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const noexcept { return path_; }
  std::filesystem::path operator/(const std::string& child) const { return path_ / child; }

 private:
  std::filesystem::path path_;
};

/// Deterministic pseudo-random generator (SplitMix64 followed by xoshiro256**).
///
/// Seeded explicitly so every randomized test is reproducible from the seed the
/// framework prints.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept;

  std::uint64_t next() noexcept;
  /// Uniform value in [0, bound).
  std::uint64_t below(std::uint64_t bound) noexcept;
  /// Uniform value in [low, high].
  std::uint64_t between(std::uint64_t low, std::uint64_t high) noexcept;
  bool coin() noexcept { return (next() & 1U) != 0; }

 private:
  std::uint64_t state_[4];
};

/// Convenience builders for canonical identifiers.
fcr::ScopeRef scope(const char* text);
fcr::SnapshotRef snapshot_ref(const char* text);
fcr::FacilityRef facility_ref(const char* text);
fcr::ClaimantRef claimant(const char* text);
fcr::ActorRef actor(const char* text);
fcr::TenantRef tenant(const char* text);
fcr::ServiceRef service(const char* text);
fcr::PriorityRef priority(const char* text);
fcr::PolicyRef policy(const char* text);
fcr::AttemptId attempt(const char* text);
fcr::ReservationId reservation_id(const char* text);

/// One pool of a synthetic snapshot.
struct PoolSpec {
  fcr::ResourceKind kind;
  const char* scope_name;
  std::uint64_t gross;
  std::uint64_t withdrawn;
  std::uint64_t floor;
};

/// Builds a canonical snapshot from pool specifications, sorted by pool key.
fcr::CapacitySnapshot make_snapshot(const char* ref, const char* facility, std::uint64_t generation,
                                    std::uint64_t captured_tick, const std::vector<PoolSpec>& pools);

/// A claim on one pool.
fcr::ResourceClaim claim(fcr::ResourceKind kind, const char* scope_name, std::uint64_t amount);

/// An in-memory store with capacity installed and a default epoch, ready for
/// requests. Fails the calling test when setup does not succeed.
struct Fixture {
  fcr::Store store;
  fcr::AuthorityEpoch epoch;
  fcr::CapacitySnapshot snapshot;

  static Fixture make(const std::vector<PoolSpec>& pools, std::uint64_t seed = 1);
};

/// Fills in the common fields of a reserve request so tests only state what they
/// are about.
fcr::ReserveRequest reserve_request(const Fixture& fixture, const char* id, const char* attempt_name,
                                    std::vector<fcr::ResourceClaim> claims, std::uint64_t start, std::uint64_t deadline);

/// Fills in the common fields of an amend request.
fcr::AmendRequest amend_request(const Fixture& fixture, const char* id, const char* attempt_name,
                                fcr::ReservationGeneration generation, std::vector<fcr::ResourceClaim> claims,
                                std::uint64_t start, std::uint64_t deadline);

/// Fills in the common fields of a release request.
fcr::ReleaseRequest release_request(const Fixture& fixture, const char* id, const char* attempt_name,
                                    fcr::ReservationGeneration generation);

/// Fills in the common fields of a revoke request.
fcr::RevokeRequest revoke_request(const Fixture& fixture, const char* id, const char* attempt_name,
                                  fcr::ReservationGeneration generation);

/// Fills in the common fields of an expire request.
fcr::ExpireRequest expire_request(const Fixture& fixture, const char* attempt_name, std::uint64_t now);

/// Fills in the common fields of a reconcile request.
fcr::ReconcileRequest reconcile_request(const Fixture& fixture, const char* attempt_name,
                                        const fcr::CapacitySnapshot& snapshot,
                                        fcr::SourceGeneration expected_source_generation,
                                        fcr::ReconcileMode mode);

/// One commitment as the reference model sees it.
struct ReferenceCommitment {
  fcr::PoolKey pool;
  std::uint64_t amount = 0;
  bool committed_bucket = true;
  bool holds_capacity = true;
};

/// Records what one pool looked like according to the reference model.
struct ReferencePool {
  fcr::PoolKey pool;
  std::uint64_t reservable = 0;
  std::uint64_t committed = 0;
  std::uint64_t protected_units = 0;
  std::uint64_t free_units = 0;
  std::size_t active = 0;
};

/// Independent recomputation of the accounting.
///
/// Returns false and fills `why` when the commitments do not fit, when a pool is
/// missing from the snapshot, or when a total overflows. On success `out` holds
/// one entry per snapshot pool in canonical order.
bool reference_accounting(const fcr::CapacitySnapshot& snapshot, const std::vector<ReferenceCommitment>& commitments,
                          std::vector<ReferencePool>& out, std::string& why);

/// Compares the library's pool accounts with the reference model and fails the
/// calling test when they disagree.
void check_accounting_matches_reference(const fcr::Store& store, const fcr::CapacitySnapshot& snapshot,
                                        const std::vector<ReferenceCommitment>& commitments);

/// Builds reference commitments from a store's own views. Used to compare two
/// independent traversals of the same published state.
std::vector<ReferenceCommitment> reference_from_views(const std::vector<fcr::ReservationView>& views);

/// Looks a pool account up by kind and scope.
///
/// The accounts are returned in canonical pool order, which is by resource kind
/// and then by scope; a test that indexed them positionally would be asserting on
/// the ordering of the kind enumeration rather than on the accounting it means to
/// check.
const fcr::PoolAccount* find_pool(const std::vector<fcr::PoolAccount>& accounts, fcr::ResourceKind kind,
                                  const char* scope_name);

/// Reads a file as text, failing the calling test when it cannot be read.
std::string read_text_file(const std::filesystem::path& path);

/// Returns its argument unchanged.
///
/// The compiler cannot fold this call, which keeps an assertion about a
/// compile-time constant from being reported as a constant conditional
/// expression. It is used only where a test deliberately asserts on a constant.
std::uint64_t runtime_u64(std::uint64_t value) noexcept;
bool runtime_bool(bool value) noexcept;

/// Writes a file, failing the calling test when it cannot be written.
void write_text_file(const std::filesystem::path& path, const std::string& content);

/// File names in a directory, sorted.
std::vector<std::string> list_names(const std::filesystem::path& directory);

/// True when a file exists.
bool file_exists(const std::filesystem::path& path);

/// Replaces the first occurrence of `needle` in a file, failing the calling test
/// when the needle is absent.
void patch_file(const std::filesystem::path& path, const std::string& needle, const std::string& replacement);

/// Replaces the first occurrence of `needle` in a string, failing the calling
/// test when the needle is absent.
void patch_text(std::string& text, const std::string& needle, const std::string& replacement);

/// Appends text to a file.
void append_file(const std::filesystem::path& path, const std::string& content);

/// The single generation file referenced by a store's head pointer.
std::filesystem::path committed_state_file(const std::filesystem::path& store_root);

/// Truncates a file to `bytes` bytes.
void truncate_file(const std::filesystem::path& path, std::uintmax_t bytes);

}  // namespace fcr_test

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_TESTS_TEST_SUPPORT_HPP
