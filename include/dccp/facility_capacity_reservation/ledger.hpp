// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The reservation ledger: the authoritative in-memory state of one facility
// capacity reservation authority.
//
// Ownership and locking
// ---------------------
// A ReservationLedger is a value type and owns no locks. Every mutating entry
// point is a staged operation: it validates the request, applies the whole
// change to a scratch copy and only then adopts the scratch copy. A failed
// operation therefore leaves the ledger byte-for-byte as it was, which is what
// makes multi-resource atomicity and crash-safe publication possible: a durable
// caller can hold the staged copy, publish it, and only then adopt it.
//
// The durable store (store.hpp) owns the only lock in the library and is the
// only component that calls the staged operations.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_LEDGER_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_LEDGER_HPP

#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dccp/facility_capacity_reservation/capacity.hpp"
#include "dccp/facility_capacity_reservation/request.hpp"
#include "dccp/facility_capacity_reservation/reservation.hpp"
#include "dccp/facility_capacity_reservation/status.hpp"
#include "dccp/facility_capacity_reservation/strong_id.hpp"

namespace dccp::facility_capacity_reservation {

/// How the ledger came by the capacity evidence it holds.
enum class CapacityOrigin : std::uint8_t {
  /// Installed from a freshly consumed snapshot in this process.
  Consumed = 1,
  /// Restored from persistence. Restored evidence is no longer known to be
  /// current, so capacity-consuming operations refuse it.
  Restored = 2,
};

std::string_view capacity_origin_token(CapacityOrigin origin) noexcept;
Result<CapacityOrigin> parse_capacity_origin(std::string_view token);

/// Operation kinds recorded in the durable attempt index.
enum class OperationKind : std::uint8_t {
  Reserve = 1,
  Amend = 2,
  Release = 3,
  Expire = 4,
  Revoke = 5,
  Reconcile = 6,
};

std::string_view operation_kind_token(OperationKind kind) noexcept;
Result<OperationKind> parse_operation_kind(std::string_view token);

/// One durable attempt record.
///
/// The record is what makes a retry safe: it binds an attempt identity to the
/// exact intent digest it carried and to the state revision it produced. A
/// repeated attempt with the same intent replays the recorded outcome; a
/// repeated attempt with a different intent is refused.
struct AttemptRecord {
  OperationKind operation = OperationKind::Reserve;
  ReservationId subject;  // empty for sweeps and reconciliation
  std::string intent_digest;
  Revision revision;
  AuthorityEpoch epoch;
  ReservationGeneration generation;  // resulting generation, when addressed
  SourceGeneration previous_source_generation;  // reconciliation only
  Tick at_tick;
};

/// Bounds and identity of a ledger.
struct LedgerOptions {
  RequestLimits limits;
  /// Maximum number of attempt records retained for replay. The index is a
  /// bounded, oldest-first cache: an evicted attempt is no longer replayable and
  /// a retry of it is refused as a conflict rather than applied twice.
  std::size_t max_attempts = 4096;
  /// Maximum number of reservation identities the ledger will hold.
  std::size_t max_reservations = 100'000;
  /// Maximum number of pool accounts derived from one capacity snapshot.
  std::size_t max_pools = 4096;
};

/// Authoritative reservation state.
class ReservationLedger {
 public:
  ReservationLedger() = default;

  /// A ledger with the given identity, authority epoch and bounds, holding no
  /// capacity and no reservations.
  static Result<ReservationLedger> create(Incarnation incarnation, AuthorityEpoch epoch, LedgerOptions options);

  // --- Capacity evidence --------------------------------------------------

  /// Installs the initial capacity evidence. Refused when the ledger already
  /// holds capacity: after that, capacity changes go through reconcile() so that
  /// existing commitments are re-examined instead of silently invalidated.
  Result<void> install_capacity(const CapacitySnapshot& snapshot, CapacityOrigin origin);

  // --- Mutations (staged) -------------------------------------------------

  /// Applies `request` to `target`, which must be a scratch copy. On failure
  /// `target` is left unspecified; on success it holds the new authoritative
  /// state and `outcome` describes what happened.
  static Result<void> stage_reserve(const ReserveRequest& request, ReservationLedger& target, ReserveOutcome& outcome);
  static Result<void> stage_amend(const AmendRequest& request, ReservationLedger& target, AmendOutcome& outcome);
  static Result<void> stage_release(const ReleaseRequest& request, ReservationLedger& target, ReleaseOutcome& outcome);
  static Result<void> stage_revoke(const RevokeRequest& request, ReservationLedger& target, RevokeOutcome& outcome);
  static Result<void> stage_expire(const ExpireRequest& request, ReservationLedger& target, ExpireOutcome& outcome);
  static Result<void> stage_reconcile(const ReconcileRequest& request, ReservationLedger& target,
                                      ReconcileOutcome& outcome);

  // --- Mutations (self-staged convenience) --------------------------------
  //
  // These copy the ledger, run the matching staged operation and adopt the copy
  // only when it succeeded, so a failed call leaves the ledger unchanged.

  Result<ReserveOutcome> reserve(const ReserveRequest& request);
  Result<AmendOutcome> amend(const AmendRequest& request);
  Result<ReleaseOutcome> release(const ReleaseRequest& request);
  Result<RevokeOutcome> revoke(const RevokeRequest& request);
  Result<ExpireOutcome> expire(const ExpireRequest& request);
  Result<ReconcileOutcome> reconcile(const ReconcileRequest& request);

  // --- Observation --------------------------------------------------------

  /// Classifies every held reservation against the capacity evidence currently
  /// installed. Publishes nothing and changes nothing.
  Result<RevalidationReport> revalidate(const RevalidateRequest& request) const;

  /// Recomputes the accounting from the records and compares it with the
  /// recorded split, and re-checks every structural invariant.
  Result<VerificationReport> verify() const;

  std::optional<ReservationView> find(const ReservationId& id) const;
  std::vector<ReservationView> list() const;  // identifier order
  std::vector<PoolAccount> pool_accounts() const;
  Result<CapacitySplit> accounting_of(const PoolKey& key) const;

  Revision revision() const noexcept { return revision_; }
  AuthorityEpoch epoch() const noexcept { return epoch_; }
  void set_epoch(AuthorityEpoch epoch) noexcept { epoch_ = epoch; }
  Incarnation incarnation() const noexcept { return incarnation_; }
  const CapacitySnapshot& capacity() const noexcept { return capacity_; }
  bool has_capacity() const noexcept { return has_capacity_; }
  CapacityOrigin capacity_origin() const noexcept { return capacity_origin_; }
  bool capacity_fresh() const noexcept { return has_capacity_ && capacity_origin_ == CapacityOrigin::Consumed; }
  const LedgerOptions& options() const noexcept { return options_; }

  std::size_t reservation_count() const noexcept { return records_.size(); }
  std::size_t active_count() const noexcept;
  std::size_t attempt_count() const noexcept { return attempts_.size(); }
  std::size_t pool_count() const noexcept { return accounting_.size(); }

  LedgerStatus status(bool durable, bool closed) const;

  /// The current tick as most recently observed by any operation.
  Tick last_observed_tick() const noexcept { return last_observed_tick_; }

  /// Records that the authority observed the logical tick `now`. Only the
  /// durable store calls this, when it publishes a state that was reached
  /// without a mutation (installing the first capacity evidence).
  void note_observed_tick(Tick now) noexcept { observe_tick(now); }

  // --- Persistence view ---------------------------------------------------
  //
  // The canonical codec reads and writes the ledger through these accessors so
  // that the persisted document is exactly the authoritative state and nothing
  // else. They expose no way to mutate the ledger.

  const std::map<ReservationId, ReservationRecord>& records_for_persistence() const noexcept { return records_; }
  const std::map<AttemptId, AttemptRecord>& attempts_for_persistence() const noexcept { return attempts_; }
  const std::deque<AttemptId>& attempt_order_for_persistence() const noexcept { return attempt_order_; }
  const std::map<PoolKey, CapacitySplit>& accounting_for_persistence() const noexcept { return accounting_; }

  /// Rebuilds a ledger from persisted parts. Only the codec calls this.
  static Result<ReservationLedger> reassemble(Incarnation incarnation, AuthorityEpoch epoch, LedgerOptions options,
                                              CapacitySnapshot capacity, bool has_capacity, CapacityOrigin origin,
                                              Revision revision,
                                              std::map<ReservationId, ReservationRecord> records,
                                              std::map<AttemptId, AttemptRecord> attempts,
                                              std::deque<AttemptId> attempt_order);

  /// Recomputes and adopts the accounting map, reporting any closure failure.
  Result<void> rebuild_accounting();

 private:
  /// Records the tick an operation observed, so that inspection can report the
  /// most recent logical time the ledger acted on.
  void observe_tick(Tick now) noexcept {
    if (now > last_observed_tick_) {
      last_observed_tick_ = now;
    }
  }

  /// Projects a stored record for callers.
  ReservationView view_of(const ReservationRecord& record) const;

  /// Increments the revision, refusing to wrap.
  Result<void> advance_revision();
  Result<void> advance_revision_to(Revision revision);

  /// Records an attempt, evicting the oldest entries beyond the bound.
  Result<void> remember_attempt(const AttemptId& attempt, const AttemptRecord& record);

  /// Looks up an attempt and decides whether it replays, conflicts or is new.
  enum class AttemptVerdict { New, Replay, Conflict };
  AttemptVerdict classify_attempt(const AttemptId& attempt, OperationKind operation, const std::string& intent_digest,
                                  const AttemptRecord*& record) const;

  /// Sums a claim set into `deltas`, checking bounds and duplicate pools.
  Result<void> accumulate_claims(const std::vector<ResourceClaim>& claims,
                                 std::map<PoolKey, Units>& deltas) const;

  /// Validates that every claim names a pool present in the held capacity.
  Result<void> require_pools_known(const std::vector<ResourceClaim>& claims) const;

  Incarnation incarnation_;
  AuthorityEpoch epoch_;
  Revision revision_;
  LedgerOptions options_;
  Tick last_observed_tick_;

  CapacitySnapshot capacity_;
  bool has_capacity_ = false;
  CapacityOrigin capacity_origin_ = CapacityOrigin::Consumed;

  std::map<ReservationId, ReservationRecord> records_;
  std::map<PoolKey, CapacitySplit> accounting_;
  std::map<AttemptId, AttemptRecord> attempts_;
  std::deque<AttemptId> attempt_order_;
};

/// Digest of the intent carried by a request. Two requests with the same
/// operation and the same intent digest are the same request; the observation
/// tick and the configured limits are deliberately excluded, so a retry issued
/// later still replays.
std::string intent_digest(const ReserveRequest& request);
std::string intent_digest(const AmendRequest& request);
std::string intent_digest(const ReleaseRequest& request);
std::string intent_digest(const RevokeRequest& request);
std::string intent_digest(const ExpireRequest& request);
std::string intent_digest(const ReconcileRequest& request);

}  // namespace dccp::facility_capacity_reservation

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_LEDGER_HPP
