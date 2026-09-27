// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The durable reservation store.
//
// Concurrency and lock order
// --------------------------
// A store holds two locks, and only two:
//
//   1. an advisory inter-process file lock, acquired by create()/open()/
//      open_read_only() and held for the whole lifetime of the store object.
//      It is never acquired while any other lock is held, and it is never
//      released before the store is closed. A process that dies holding it
//      releases it through the operating system, which is what makes writer
//      handover after a crash possible.
//
//   2. one process-local, non-recursive std::mutex that serialises mutations
//      within the process.
//
// The lock order is therefore: file lock (already held, never re-acquired) then
// mutex. Nothing in this library acquires the file lock while holding the mutex,
// and nothing acquires the mutex twice. The library invokes no caller-supplied
// callback anywhere, so a callback cannot re-enter a locked store; there is no
// observer, no listener and no event sink in the public surface.
//
// Publication protocol
// --------------------
// A mutation is staged on a copy of the ledger. If it succeeds, the new state is
// written to a uniquely named generation file, flushed, read back and verified,
// and only then does an atomically replaced head pointer make it current. Only
// after the head commit does the store adopt the staged ledger. A crash at any
// point therefore leaves either the previous committed state or the new one, and
// never a mixture; the in-memory state can never be ahead of the durable one.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_STORE_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_STORE_HPP

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dccp/facility_capacity_reservation/capacity.hpp"
#include "dccp/facility_capacity_reservation/ledger.hpp"
#include "dccp/facility_capacity_reservation/request.hpp"
#include "dccp/facility_capacity_reservation/reservation.hpp"
#include "dccp/facility_capacity_reservation/status.hpp"
#include "dccp/facility_capacity_reservation/strong_id.hpp"

namespace dccp::facility_capacity_reservation {

/// Whether a store publishes to disk.
enum class DurabilityMode : std::uint8_t {
  /// Every accepted mutation is durable before the call returns.
  Durable = 1,
  /// The store lives only in this process. Used by examples and benchmarks; it
  /// is reported honestly as non-durable by status().
  Volatile = 2,
};

std::string_view durability_mode_token(DurabilityMode mode) noexcept;

/// Bounds applied to everything the store reads from disk.
struct StoreLimits {
  std::size_t max_state_bytes = kMaxDocumentBytes;
  std::size_t max_meta_bytes = 8192;
  std::size_t max_head_bytes = 8192;
  std::size_t max_state_lines = 4'000'000;
  std::size_t max_generation_files = 4096;
};

/// Configuration of a store.
struct StoreOptions {
  LedgerOptions ledger;
  StoreLimits limits;
  DurabilityMode durability = DurabilityMode::Durable;

  /// When true, capacity evidence restored from persistence is treated as
  /// current. The default is false: restored evidence is real capacity but it is
  /// no longer known to be current, so capacity-consuming operations refuse it
  /// until a fresh snapshot is consumed through reconcile().
  bool trust_persisted_capacity = false;
};

/// What happened while a store directory was opened.
struct RecoveryReport {
  bool opened_existing = false;
  /// True when recovery had work to do (orphan generations, or an epoch
  /// handover).
  bool recovered = false;
  AuthorityEpoch previous_epoch;
  AuthorityEpoch current_epoch;
  std::size_t orphan_generations_removed = 0;
  std::size_t cleanup_failures = 0;
  bool capacity_restored = false;
  Revision revision;
};

/// Authoritative facility-capacity reservation state, optionally durable.
class Store {
 public:
  Store() noexcept;
  ~Store();

  Store(Store&& other) noexcept;
  Store& operator=(Store&& other) noexcept;
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  /// Creates a store in an empty (or absent) directory.
  static Result<Store> create(const std::filesystem::path& root, const StoreOptions& options = StoreOptions());

  /// Opens an existing store for writing and takes write authority over it.
  ///
  /// The writer authority epoch is advanced and durably published before the
  /// store accepts any mutation, so a decision taken under a previous epoch is
  /// refused rather than merged.
  static Result<Store> open(const std::filesystem::path& root, const StoreOptions& options = StoreOptions());

  /// Opens an existing store for observation only, under a shared lock.
  ///
  /// Every mutating call is refused with STORE_READ_ONLY. A read-only handle is
  /// still at least as strict as the writing path: it verifies the integrity of
  /// everything it reads.
  static Result<Store> open_read_only(const std::filesystem::path& root,
                                      const StoreOptions& options = StoreOptions());

  /// A store that lives only in this process.
  static Result<Store> in_memory(const StoreOptions& options = StoreOptions());

  /// Releases write authority and closes the store. Idempotent.
  void close() noexcept;

  bool closed() const noexcept;
  bool read_only() const noexcept;
  bool durable() const noexcept;
  const std::filesystem::path& root() const noexcept;
  RecoveryReport recovery() const;

  // --- Capacity evidence --------------------------------------------------

  /// Installs the initial capacity evidence and publishes it.
  Result<void> install_capacity(const CapacitySnapshot& snapshot, Tick now);

  // --- Mutations ----------------------------------------------------------

  Result<ReserveOutcome> reserve(const ReserveRequest& request);
  Result<AmendOutcome> amend(const AmendRequest& request);
  Result<ReleaseOutcome> release(const ReleaseRequest& request);
  Result<RevokeOutcome> revoke(const RevokeRequest& request);
  Result<ExpireOutcome> expire(const ExpireRequest& request);
  Result<ReconcileOutcome> reconcile(const ReconcileRequest& request);

  // --- Observation --------------------------------------------------------

  Result<RevalidationReport> revalidate(const RevalidateRequest& request) const;
  Result<VerificationReport> verify() const;
  Result<LedgerStatus> status() const;
  Result<std::optional<ReservationView>> find(const ReservationId& id) const;
  Result<std::vector<ReservationView>> list() const;
  Result<std::vector<PoolAccount>> pools() const;
  Result<CapacitySplit> accounting_of(const PoolKey& key) const;
  Result<CapacitySnapshot> capacity() const;
  Result<Tick> last_observed_tick() const;

  /// Writer authority epoch the store currently holds.
  Result<AuthorityEpoch> epoch() const;
  /// Current published state revision.
  Result<Revision> revision() const;
  /// Identity of the store incarnation.
  Result<Incarnation> incarnation() const;

 private:
  static Result<Store> open_impl(const std::filesystem::path& root, const StoreOptions& options, bool read_only);

  /// Staged mutation helper.
  ///
  /// `apply` receives a scratch ledger. On success the scratch copy is published
  /// (when it moved the revision) and only then adopted, so a failure — including
  /// a failure to publish — leaves the authoritative ledger exactly as it was.
  template <class Apply>
  auto mutate(Apply&& apply) -> decltype(apply(std::declval<ReservationLedger&>())) {
    using Outcome = typename decltype(apply(std::declval<ReservationLedger&>()))::value_type;

    std::lock_guard<std::mutex> guard(*mutex_);
    FCR_TRYV(require_writable_locked());

    ReservationLedger staged = ledger_;
    Result<Outcome> result = apply(staged);
    if (!result.has_value()) {
      return result;
    }
    // A volatile store has no directory, no head pointer and no commit point: it
    // must not publish anything anywhere. Gating on durability here as well as in
    // publish_locked() is deliberate — a mutation path that forgot the gate would
    // silently write state generations into the process's working directory.
    if (durable_ && staged.revision() != ledger_.revision()) {
      FCR_TRYV(publish_locked(staged));
    }
    Outcome outcome = std::move(result).value();
    ledger_ = std::move(staged);
    return Result<Outcome>(std::move(outcome));
  }

  Result<void> require_open_locked() const;
  Result<void> require_writable_locked() const;

  /// Publishes `ledger` as the new authoritative state. Called with the process
  /// mutex held.
  Result<void> publish_locked(const ReservationLedger& ledger);

  /// Advances the writer authority epoch and publishes the handover.
  Result<void> take_authority_locked();

  /// Removes generation files that the head does not reference, counting the
  /// ones that could not be removed.
  void retire_generations_locked(std::size_t& removed, std::size_t& failures) const;

  struct FileLockHolder;
  std::unique_ptr<FileLockHolder> lock_holder_;

  // A store is movable (so factories can return one) but the mutex is not, so it
  // is held behind a pointer. A store must not be moved while another thread is
  // using it.
  std::unique_ptr<std::mutex> mutex_;
  ReservationLedger ledger_;
  StoreOptions options_;
  std::filesystem::path root_;
  std::filesystem::path generations_;
  std::string state_file_name_;
  std::string state_digest_;
  std::uint64_t state_bytes_ = 0;
  std::uint64_t head_sequence_ = 0;
  RecoveryReport recovery_;
  bool durable_ = false;
  bool read_only_ = false;
  bool closed_ = false;
};

}  // namespace dccp::facility_capacity_reservation

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_STORE_HPP
