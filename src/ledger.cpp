// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Ledger construction, capacity evidence, accounting and intent digests. The
// staged operations live in ledger_ops.cpp.

#include "dccp/facility_capacity_reservation/ledger.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "detail/ledger_internal.hpp"
#include "dccp/facility_capacity_reservation/digest.hpp"
#include "dccp/facility_capacity_reservation/text.hpp"

namespace dccp::facility_capacity_reservation {

/// Largest number of reservations one revalidation report may describe. The
/// bound is a defence against an operator pointing the report at a state file
/// that was not produced by this library.
inline constexpr std::size_t kMaxReportEntries = 4'000'000;

std::string_view capacity_origin_token(CapacityOrigin origin) noexcept {
  switch (origin) {
    case CapacityOrigin::Consumed:
      return "consumed";
    case CapacityOrigin::Restored:
      return "restored";
  }
  return "unknown";
}

Result<CapacityOrigin> parse_capacity_origin(std::string_view token) {
  if (token == "consumed") {
    return CapacityOrigin::Consumed;
  }
  if (token == "restored") {
    return CapacityOrigin::Restored;
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown capacity origin token").with_subject(sanitize_for_display(token));
}

std::string_view operation_kind_token(OperationKind kind) noexcept {
  switch (kind) {
    case OperationKind::Reserve:
      return "reserve";
    case OperationKind::Amend:
      return "amend";
    case OperationKind::Release:
      return "release";
    case OperationKind::Expire:
      return "expire";
    case OperationKind::Revoke:
      return "revoke";
    case OperationKind::Reconcile:
      return "reconcile";
  }
  return "unknown";
}

Result<OperationKind> parse_operation_kind(std::string_view token) {
  if (token == "reserve") {
    return OperationKind::Reserve;
  }
  if (token == "amend") {
    return OperationKind::Amend;
  }
  if (token == "release") {
    return OperationKind::Release;
  }
  if (token == "expire") {
    return OperationKind::Expire;
  }
  if (token == "revoke") {
    return OperationKind::Revoke;
  }
  if (token == "reconcile") {
    return OperationKind::Reconcile;
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown operation kind token").with_subject(sanitize_for_display(token));
}

// ---------------------------------------------------------------------------
// Intent digests
// ---------------------------------------------------------------------------

namespace {

/// A field that may legitimately be absent; "-" is not a legal identifier so it
/// cannot collide with a present value.
std::string_view optional_field(const std::string& value) noexcept {
  return value.empty() ? std::string_view("-") : std::string_view(value);
}

}  // namespace

std::string intent_digest(const ReserveRequest& request) {
  std::string material;
  detail::append_field(material, "reserve");
  detail::append_field(material, request.id.value());
  detail::append_field(material, request.claimant.value());
  detail::append_field(material, request.tenant.value());
  detail::append_field(material, request.service.value());
  detail::append_field(material, request.priority.value());
  detail::append_field(material, headroom_class_token(request.headroom));
  detail::append_claims(material, request.claims);
  detail::append_validity(material, request.validity);
  detail::append_field(material, format_unsigned(request.expected_source_generation.value()));
  return sha256_hex(material);
}

std::string intent_digest(const AmendRequest& request) {
  std::string material;
  detail::append_field(material, "amend");
  detail::append_field(material, request.id.value());
  detail::append_field(material, format_unsigned(request.expected_generation.value()));
  detail::append_field(material, request.actor.value());
  detail::append_claims(material, request.claims);
  detail::append_validity(material, request.validity);
  detail::append_field(material, request.priority.has_value() ? optional_field(request.priority->value())
                                                             : std::string_view("-"));
  detail::append_field(material, request.headroom.has_value() ? headroom_class_token(*request.headroom)
                                                             : std::string_view("-"));
  detail::append_field(material, format_unsigned(request.expected_source_generation.value()));
  detail::append_field(material, amendment_cause_token(request.cause));
  detail::append_field(material, request.detail);
  return sha256_hex(material);
}

std::string intent_digest(const ReleaseRequest& request) {
  std::string material;
  detail::append_field(material, "release");
  detail::append_field(material, request.id.value());
  detail::append_field(material, format_unsigned(request.expected_generation.value()));
  detail::append_field(material, request.actor.value());
  detail::append_field(material, request.detail);
  return sha256_hex(material);
}

std::string intent_digest(const RevokeRequest& request) {
  std::string material;
  detail::append_field(material, "revoke");
  detail::append_field(material, request.id.value());
  detail::append_field(material, format_unsigned(request.expected_generation.value()));
  detail::append_field(material, request.actor.value());
  detail::append_field(material, request.policy.has_value() ? optional_field(request.policy->value())
                                                           : std::string_view("-"));
  detail::append_field(material, request.allow_guaranteed_override ? "1" : "0");
  detail::append_field(material, transition_cause_token(request.cause));
  detail::append_field(material, request.detail);
  return sha256_hex(material);
}

std::string intent_digest(const ExpireRequest& request) {
  std::string material;
  detail::append_field(material, "expire");
  // The sweep's intent is the moment it sweeps at: a retry of the same attempt
  // at the same tick is the same sweep.
  detail::append_field(material, format_unsigned(request.now.value()));
  return sha256_hex(material);
}

std::string intent_digest(const ReconcileRequest& request) {
  std::string material;
  detail::append_field(material, "reconcile");
  detail::append_field(material, request.snapshot.ref.value());
  detail::append_field(material, format_unsigned(request.snapshot.source_generation.value()));
  detail::append_field(material, request.snapshot.body_digest);
  detail::append_field(material, format_unsigned(request.expected_source_generation.value()));
  detail::append_field(material, reconcile_mode_token(request.mode));
  detail::append_field(material, request.actor.value());
  detail::append_field(material, request.policy.has_value() ? optional_field(request.policy->value())
                                                           : std::string_view("-"));
  return sha256_hex(material);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Result<ReservationLedger> ReservationLedger::create(Incarnation incarnation, AuthorityEpoch epoch,
                                                    LedgerOptions options) {
  if (incarnation.empty()) {
    return Error(ErrorCode::MissingField, "a ledger requires a store incarnation");
  }
  if (epoch.is_zero()) {
    return Error(ErrorCode::MissingField, "a ledger requires a non-zero authority epoch");
  }
  if (options.max_attempts == 0) {
    return Error(ErrorCode::InvalidArgument, "the attempt index bound must be at least one");
  }
  if (options.max_reservations == 0) {
    return Error(ErrorCode::InvalidArgument, "the reservation bound must be at least one");
  }
  if (options.max_pools == 0) {
    return Error(ErrorCode::InvalidArgument, "the pool bound must be at least one");
  }
  ReservationLedger ledger;
  ledger.incarnation_ = std::move(incarnation);
  ledger.epoch_ = epoch;
  ledger.revision_ = Revision(0);
  ledger.options_ = options;
  return ledger;
}

Result<ReservationLedger> ReservationLedger::reassemble(Incarnation incarnation, AuthorityEpoch epoch,
                                                        LedgerOptions options, CapacitySnapshot capacity,
                                                        bool has_capacity, CapacityOrigin origin, Revision revision,
                                                        std::map<ReservationId, ReservationRecord> records,
                                                        std::map<AttemptId, AttemptRecord> attempts,
                                                        std::deque<AttemptId> attempt_order) {
  if (incarnation.empty()) {
    return Error(ErrorCode::MissingField, "a restored ledger requires a store incarnation");
  }
  if (records.size() > options.max_reservations) {
    return Error(ErrorCode::LimitExceeded, "the restored state holds more reservations than the configured maximum");
  }
  if (attempts.size() > options.max_attempts) {
    return Error(ErrorCode::LimitExceeded, "the restored state holds more attempts than the configured maximum");
  }
  if (has_capacity && capacity.pools.size() > options.max_pools) {
    return Error(ErrorCode::LimitExceeded, "the restored state holds more pools than the configured maximum");
  }

  ReservationLedger ledger;
  ledger.incarnation_ = std::move(incarnation);
  ledger.epoch_ = epoch;
  ledger.options_ = options;
  ledger.revision_ = revision;
  ledger.capacity_ = std::move(capacity);
  ledger.has_capacity_ = has_capacity;
  ledger.capacity_origin_ = origin;
  ledger.records_ = std::move(records);
  ledger.attempts_ = std::move(attempts);
  ledger.attempt_order_ = std::move(attempt_order);

  // A restored state is only accepted if it is internally whole: the accounting
  // is recomputed from the records rather than trusted, and every structural
  // invariant is re-checked.
  FCR_TRYV(ledger.rebuild_accounting());
  FCR_TRY(report, ledger.verify());
  if (!report.ok) {
    return Error(ErrorCode::StoreCorrupt, "the restored state does not satisfy the accounting closure");
  }
  return ledger;
}

// ---------------------------------------------------------------------------
// Capacity evidence
// ---------------------------------------------------------------------------

Result<void> ReservationLedger::install_capacity(const CapacitySnapshot& snapshot, CapacityOrigin origin) {
  if (has_capacity_) {
    return Error(ErrorCode::CapacityAlreadyInstalled,
                 "capacity evidence is already installed; use reconcile to adopt a newer snapshot");
  }
  if (!records_.empty()) {
    return Error(ErrorCode::CapacityAlreadyInstalled,
                 "capacity evidence cannot be installed once reservations exist");
  }
  FCR_TRYV(detail::validate_snapshot_shape(snapshot, options_.max_pools));
  capacity_ = snapshot;
  has_capacity_ = true;
  capacity_origin_ = origin;
  FCR_TRYV(rebuild_accounting());
  if (revision_.is_zero()) {
    revision_ = Revision(1);
  }
  return ok();
}

// ---------------------------------------------------------------------------
// Accounting
// ---------------------------------------------------------------------------

Result<void> ReservationLedger::rebuild_accounting() {
  std::map<PoolKey, CapacitySplit> fresh;
  if (has_capacity_) {
    for (const CapacityPool& pool : capacity_.pools) {
      FCR_TRY(reservable, pool.reservable());
      CapacitySplit split;
      split.free = Quantity(reservable);
      fresh.emplace(pool.key, split);
    }
  }
  for (const auto& entry : records_) {
    const ReservationRecord& record = entry.second;
    if (!reservation_state_holds_capacity(record.state)) {
      continue;
    }
    for (const ResourceClaim& claim : record.claims) {
      const auto position = fresh.find(claim.pool);
      if (position == fresh.end()) {
        return Error(ErrorCode::InvariantViolation,
                     "an active reservation claims a capacity pool that is not in the installed evidence")
            .with_subject(claim.pool.to_string());
      }
      // Claimed units are removed from free as they are accumulated: doing the
      // subtraction first is what turns an over-commit into a reported shortfall
      // instead of a silently wrapped total.
      FCR_TRY(reduced, checked_sub(position->second.free.units(), claim.amount));
      position->second.free = Quantity(reduced);
      if (headroom_counts_as_committed(record.headroom)) {
        FCR_TRY(total, checked_add(position->second.committed.units(), claim.amount));
        position->second.committed = Quantity(total);
      } else {
        FCR_TRY(total, checked_add(position->second.protected_.units(), claim.amount));
        position->second.protected_ = Quantity(total);
      }
    }
  }
  for (const auto& entry : fresh) {
    const CapacityPool* pool = capacity_.find(entry.first);
    if (pool == nullptr) {
      return Error(ErrorCode::InvariantViolation, "the accounting holds a pool that is not in the evidence")
          .with_subject(entry.first.to_string());
    }
    FCR_TRY(reservable, pool->reservable());
    const CapacitySplit& split = entry.second;
    FCR_TRY(held, checked_add(split.committed.units(), split.protected_.units()));
    FCR_TRY(total, checked_add(held, split.free.units()));
    if (total != reservable) {
      return Error(ErrorCode::AccountingMismatch,
                   "committed + protected + free does not equal the reservable capacity of the pool")
          .with_subject(entry.first.to_string());
    }
  }
  accounting_ = std::move(fresh);
  return ok();
}

// ---------------------------------------------------------------------------
// Attempt identity
// ---------------------------------------------------------------------------

Result<void> ReservationLedger::remember_attempt(const AttemptId& attempt, const AttemptRecord& record) {
  const auto existing = attempts_.find(attempt);
  if (existing != attempts_.end()) {
    // An identity that is already present keeps its position in the eviction
    // order so the bound stays a strict oldest-first cache.
    existing->second = record;
    return ok();
  }
  attempts_.emplace(attempt, record);
  attempt_order_.push_back(attempt);
  while (attempts_.size() > options_.max_attempts) {
    attempts_.erase(attempt_order_.front());
    attempt_order_.pop_front();
  }
  return ok();
}

ReservationLedger::AttemptVerdict ReservationLedger::classify_attempt(const AttemptId& attempt, OperationKind operation,
                                                                     const std::string& digest,
                                                                     const AttemptRecord*& record) const {
  const auto position = attempts_.find(attempt);
  if (position == attempts_.end()) {
    record = nullptr;
    return AttemptVerdict::New;
  }
  record = &position->second;
  if (position->second.operation != operation || position->second.intent_digest != digest) {
    return AttemptVerdict::Conflict;
  }
  return AttemptVerdict::Replay;
}

// ---------------------------------------------------------------------------
// Revision
// ---------------------------------------------------------------------------

Result<void> ReservationLedger::advance_revision() {
  FCR_TRY(next, revision_.next());
  revision_ = next;
  return ok();
}

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

std::optional<ReservationView> ReservationLedger::find(const ReservationId& id) const {
  const auto position = records_.find(id);
  if (position == records_.end()) {
    return std::nullopt;
  }
  return view_of(position->second);
}

ReservationView ReservationLedger::view_of(const ReservationRecord& record) const {
  ReservationView view;
  view.record = record;
  view.source_stale = has_capacity_ && record.source_generation != capacity_.source_generation;
  view.deadline_passed = record.state == ReservationState::Active && record.validity.elapsed_at(last_observed_tick_);
  return view;
}

std::vector<ReservationView> ReservationLedger::list() const {
  std::vector<ReservationView> views;
  views.reserve(records_.size());
  for (const auto& entry : records_) {
    views.push_back(view_of(entry.second));
  }
  return views;
}

std::size_t ReservationLedger::active_count() const noexcept {
  std::size_t count = 0;
  for (const auto& entry : records_) {
    if (reservation_state_holds_capacity(entry.second.state)) {
      ++count;
    }
  }
  return count;
}

std::vector<PoolAccount> ReservationLedger::pool_accounts() const {
  std::vector<PoolAccount> accounts;
  accounts.reserve(accounting_.size());
  for (const auto& entry : accounting_) {
    PoolAccount account;
    account.pool = entry.first;
    const CapacityPool* pool = capacity_.find(entry.first);
    if (pool != nullptr) {
      account.gross = pool->gross;
      account.withdrawn = pool->withdrawn;
      account.floor = pool->floor;
      const Result<Units> available = pool->available();
      const Result<Units> reservable = pool->reservable();
      account.available = available.has_value() ? *available : 0;
      account.reservable = reservable.has_value() ? *reservable : 0;
    }
    account.committed = entry.second.committed.units();
    account.protected_ = entry.second.protected_.units();
    account.free = entry.second.free.units();
    accounts.push_back(account);
  }
  for (const auto& entry : records_) {
    if (!reservation_state_holds_capacity(entry.second.state)) {
      continue;
    }
    for (PoolAccount& account : accounts) {
      if (entry.second.claimed_from(account.pool) != 0) {
        ++account.active_reservations;
      }
    }
  }
  return accounts;
}

Result<CapacitySplit> ReservationLedger::accounting_of(const PoolKey& key) const {
  const auto position = accounting_.find(key);
  if (position == accounting_.end()) {
    return Error(ErrorCode::PoolUnknown, "the capacity pool is not present in the installed evidence")
        .with_subject(key.to_string());
  }
  return position->second;
}

LedgerStatus ReservationLedger::status(bool durable, bool closed) const {
  LedgerStatus status;
  status.revision = revision_;
  status.epoch = epoch_;
  status.incarnation = incarnation_;
  status.source_generation = capacity_.source_generation;
  status.source_snapshot = capacity_.ref;
  status.facility = capacity_.facility;
  status.capacity_fresh = capacity_fresh();
  status.durable = durable;
  status.closed = closed;
  status.reservation_count = records_.size();
  status.active_count = active_count();
  status.attempt_count = attempts_.size();
  status.pool_count = accounting_.size();
  return status;
}

Result<RevalidationReport> ReservationLedger::revalidate(const RevalidateRequest& request) const {
  if (request.expected_revision != revision_) {
    return Error(ErrorCode::StaleRevision, "the revalidation was requested against a different state revision")
        .with_subject("expected=" + format_unsigned(request.expected_revision.value()) +
                      " current=" + format_unsigned(revision_.value()));
  }
  if (request.now.is_zero()) {
    return Error(ErrorCode::MissingField, "a revalidation requires the logical tick it observes at");
  }
  if (records_.size() > kMaxReportEntries) {
    return Error(ErrorCode::LimitExceeded, "the ledger holds more reservations than one report may describe");
  }

  RevalidationReport report;
  report.revision = revision_;
  report.now = request.now;
  report.epoch = epoch_;
  report.source_generation = has_capacity_ ? capacity_.source_generation : SourceGeneration(0);
  report.entries.reserve(records_.size());

  for (const auto& entry : records_) {
    const ReservationRecord& record = entry.second;
    if (!reservation_state_holds_capacity(record.state)) {
      continue;
    }
    RevalidationEntry finding;
    finding.id = record.id;
    finding.generation = record.generation;

    // Classification precedence is fixed and documented: a reservation that has
    // run out of time is reported as such before any statement about capacity,
    // because the deadline is a property of the commitment itself.
    if (record.validity.elapsed_at(request.now)) {
      finding.classification = RevalidationClass::DeadlinePassed;
      finding.detail = "validity deadline " + format_unsigned(record.validity.deadline.value()) + " has elapsed";
      ++report.deadline_passed_count;
    } else if (!has_capacity_) {
      finding.classification = RevalidationClass::PoolMissing;
      finding.detail = "no capacity evidence is installed";
      ++report.pool_missing_count;
    } else if (record.source_generation != capacity_.source_generation) {
      finding.classification = RevalidationClass::SourceGenerationStale;
      finding.detail = "priced against source generation " + format_unsigned(record.source_generation.value()) +
                       " but the held evidence is generation " + format_unsigned(capacity_.source_generation.value());
      ++report.stale_count;
    } else {
      bool missing = false;
      bool exceeded = false;
      std::string detail;
      for (const ResourceClaim& claim : record.claims) {
        const CapacityPool* pool = capacity_.find(claim.pool);
        if (pool == nullptr) {
          missing = true;
          detail = "pool " + claim.pool.to_string() + " is absent from the held capacity evidence";
          break;
        }
        const Result<Units> reservable = pool->reservable();
        const Units limit = reservable.has_value() ? *reservable : 0;
        if (claim.amount > limit) {
          exceeded = true;
          detail = "pool " + claim.pool.to_string() + " claims " + format_unsigned(claim.amount) +
                   " units but only " + format_unsigned(limit) + " are reservable";
          break;
        }
      }
      if (missing) {
        finding.classification = RevalidationClass::PoolMissing;
        ++report.pool_missing_count;
      } else if (exceeded) {
        finding.classification = RevalidationClass::CapacityExceeded;
        ++report.capacity_exceeded_count;
      } else {
        finding.classification = RevalidationClass::Current;
        ++report.current_count;
      }
      finding.detail = std::move(detail);
    }
    report.entries.push_back(std::move(finding));
  }
  return report;
}

Result<VerificationReport> ReservationLedger::verify() const {
  VerificationReport report;
  report.revision = revision_;
  report.epoch = epoch_;
  report.source_generation = has_capacity_ ? capacity_.source_generation : SourceGeneration(0);
  report.reservation_count = records_.size();
  report.active_count = 0;
  report.attempt_count = attempts_.size();
  report.pool_count = accounting_.size();

  // Structural invariants of every record.
  for (const auto& entry : records_) {
    const ReservationRecord& record = entry.second;
    if (record.id != entry.first) {
      return Error(ErrorCode::InvariantViolation, "a reservation record is filed under the wrong identity")
          .with_subject(entry.first.value());
    }
    if (record.generation.is_zero()) {
      return Error(ErrorCode::InvariantViolation, "a reservation record has no generation")
          .with_subject(record.id.value());
    }
    if (record.claims.empty()) {
      return Error(ErrorCode::InvariantViolation, "a reservation record has no claims").with_subject(record.id.value());
    }
    for (std::size_t index = 0; index < record.claims.size(); ++index) {
      if (record.claims[index].amount == 0) {
        return Error(ErrorCode::InvariantViolation, "a reservation record holds a zero claim")
            .with_subject(record.id.value());
      }
      if (index > 0 && !(record.claims[index - 1].pool < record.claims[index].pool)) {
        return Error(ErrorCode::InvariantViolation, "a reservation record's claims are not canonical")
            .with_subject(record.id.value());
      }
    }
    FCR_TRYV(record.validity.validate());
    if (reservation_state_holds_capacity(record.state)) {
      ++report.active_count;
      if (record.termination.has_value()) {
        return Error(ErrorCode::InvariantViolation, "an active reservation carries termination provenance")
            .with_subject(record.id.value());
      }
    } else if (!record.termination.has_value()) {
      return Error(ErrorCode::InvariantViolation, "a terminal reservation carries no termination provenance")
          .with_subject(record.id.value());
    }
    ReservationGeneration expected = ReservationGeneration::first();
    for (const AmendmentEntry& line : record.lineage) {
      if (line.predecessor != expected) {
        return Error(ErrorCode::InvariantViolation, "the amendment lineage is not a contiguous chain")
            .with_subject(record.id.value());
      }
      FCR_TRY(next, expected.next());
      expected = next;
      if (line.generation != expected) {
        return Error(ErrorCode::InvariantViolation, "the amendment lineage skips a generation")
            .with_subject(record.id.value());
      }
    }
    if (record.generation != expected) {
      return Error(ErrorCode::InvariantViolation,
                   "the reservation generation does not match the length of its amendment lineage")
          .with_subject(record.id.value());
    }
    if (record.lineage.size() > options_.limits.max_lineage_entries) {
      return Error(ErrorCode::InvariantViolation, "the amendment lineage exceeds the configured bound")
          .with_subject(record.id.value());
    }
  }

  // The attempt index must contain exactly the identities it claims to order.
  if (attempt_order_.size() != attempts_.size()) {
    return Error(ErrorCode::InvariantViolation, "the attempt index and its eviction order disagree in size");
  }
  {
    std::vector<AttemptId> sorted(attempt_order_.begin(), attempt_order_.end());
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
      return Error(ErrorCode::InvariantViolation, "the attempt eviction order holds a duplicate identity");
    }
    for (const AttemptId& attempt : attempt_order_) {
      if (attempts_.find(attempt) == attempts_.end()) {
        return Error(ErrorCode::InvariantViolation, "the attempt eviction order names an unknown attempt")
            .with_subject(attempt.value());
      }
    }
  }

  // Independent recomputation of the accounting, deliberately written as a
  // separate traversal from rebuild_accounting() so that a defect in one is not
  // reproduced by the other.
  std::map<PoolKey, CapacitySplit> recomputed;
  if (has_capacity_) {
    for (const CapacityPool& pool : capacity_.pools) {
      FCR_TRY(reservable, pool.reservable());
      CapacitySplit split;
      split.free = Quantity(reservable);
      recomputed.emplace(pool.key, split);
    }
  }
  for (const auto& entry : records_) {
    if (!reservation_state_holds_capacity(entry.second.state)) {
      continue;
    }
    const bool committed_bucket = headroom_counts_as_committed(entry.second.headroom);
    for (const ResourceClaim& claim : entry.second.claims) {
      const auto position = recomputed.find(claim.pool);
      if (position == recomputed.end()) {
        return Error(ErrorCode::InvariantViolation, "an active reservation claims an unknown pool")
            .with_subject(claim.pool.to_string());
      }
      FCR_TRY(reduced, checked_sub(position->second.free.units(), claim.amount));
      position->second.free = Quantity(reduced);
      if (committed_bucket) {
        FCR_TRY(total, checked_add(position->second.committed.units(), claim.amount));
        position->second.committed = Quantity(total);
      } else {
        FCR_TRY(total, checked_add(position->second.protected_.units(), claim.amount));
        position->second.protected_ = Quantity(total);
      }
    }
  }

  if (recomputed.size() != accounting_.size()) {
    report.ok = false;
    for (const auto& entry : recomputed) {
      if (accounting_.find(entry.first) == accounting_.end()) {
        report.mismatched_pools.push_back(entry.first);
      }
    }
    for (const auto& entry : accounting_) {
      if (recomputed.find(entry.first) == recomputed.end()) {
        report.mismatched_pools.push_back(entry.first);
      }
    }
    std::sort(report.mismatched_pools.begin(), report.mismatched_pools.end());
    return report;
  }
  for (const auto& entry : recomputed) {
    const auto position = accounting_.find(entry.first);
    if (position == accounting_.end() || position->second.committed != entry.second.committed ||
        position->second.protected_ != entry.second.protected_ || position->second.free != entry.second.free) {
      report.mismatched_pools.push_back(entry.first);
    }
  }
  report.ok = report.mismatched_pools.empty();
  return report;
}

// ---------------------------------------------------------------------------
// Self-staged convenience wrappers
// ---------------------------------------------------------------------------

Result<ReserveOutcome> ReservationLedger::reserve(const ReserveRequest& request) {
  ReservationLedger staged = *this;
  ReserveOutcome outcome;
  FCR_TRYV(stage_reserve(request, staged, outcome));
  *this = std::move(staged);
  return outcome;
}

Result<AmendOutcome> ReservationLedger::amend(const AmendRequest& request) {
  ReservationLedger staged = *this;
  AmendOutcome outcome;
  FCR_TRYV(stage_amend(request, staged, outcome));
  *this = std::move(staged);
  return outcome;
}

Result<ReleaseOutcome> ReservationLedger::release(const ReleaseRequest& request) {
  ReservationLedger staged = *this;
  ReleaseOutcome outcome;
  FCR_TRYV(stage_release(request, staged, outcome));
  *this = std::move(staged);
  return outcome;
}

Result<RevokeOutcome> ReservationLedger::revoke(const RevokeRequest& request) {
  ReservationLedger staged = *this;
  RevokeOutcome outcome;
  FCR_TRYV(stage_revoke(request, staged, outcome));
  *this = std::move(staged);
  return outcome;
}

Result<ExpireOutcome> ReservationLedger::expire(const ExpireRequest& request) {
  ReservationLedger staged = *this;
  ExpireOutcome outcome;
  FCR_TRYV(stage_expire(request, staged, outcome));
  *this = std::move(staged);
  return outcome;
}

Result<ReconcileOutcome> ReservationLedger::reconcile(const ReconcileRequest& request) {
  ReservationLedger staged = *this;
  ReconcileOutcome outcome;
  FCR_TRYV(stage_reconcile(request, staged, outcome));
  *this = std::move(staged);
  return outcome;
}

}  // namespace dccp::facility_capacity_reservation
