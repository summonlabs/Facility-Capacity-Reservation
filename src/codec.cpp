// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "detail/codec.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dccp/facility_capacity_reservation/digest.hpp"
#include "dccp/facility_capacity_reservation/text.hpp"

namespace dccp::facility_capacity_reservation::detail {
namespace {

constexpr std::string_view kDigestPrefix = "digest=sha256:";
constexpr std::string_view kAbsent = "-";

std::string_view present(std::string_view value) noexcept { return value.empty() ? kAbsent : value; }

void append_key(std::string& out, std::string_view key) {
  out.append(key);
  out.push_back('=');
}

void append_line(std::string& out, std::string_view key, std::string_view value) {
  append_key(out, key);
  out.append(value);
  out.push_back('\n');
}

void append_number(std::string& out, std::string_view key, std::uint64_t value) {
  append_line(out, key, format_unsigned(value));
}

Result<void> encode_detail(std::string& out, std::string_view key, std::string_view value,
                           std::size_t max_bytes) {
  FCR_TRY(encoded, encode_field(value, max_bytes));
  append_line(out, key, encoded);
  return ok();
}

/// A strict cursor over the decoded lines of a state document.
class Cursor {
 public:
  Cursor(const std::vector<std::string_view>& lines, std::size_t end) : lines_(lines), end_(end) {}

  bool at_end() const noexcept { return index_ >= end_; }
  std::size_t remaining() const noexcept { return end_ - index_; }
  std::size_t index() const noexcept { return index_; }

  /// Consumes the next line, requiring an exact key.
  Result<std::string_view> take(std::string_view key) {
    if (index_ >= end_) {
      return Error(ErrorCode::TruncatedInput, "the state document ends before field \"" + std::string(key) + "\"");
    }
    const std::string_view line = lines_[index_];
    std::string prefix(key);
    prefix.push_back('=');
    if (line.size() < prefix.size() || line.substr(0, prefix.size()) != prefix) {
      return Error(ErrorCode::MalformedRecord,
                   "expected field \"" + std::string(key) + "\" but found a different key");
    }
    ++index_;
    return line.substr(prefix.size());
  }

  Result<std::uint64_t> take_number(std::string_view key) {
    FCR_TRY(text, take(key));
    return parse_unsigned(text);
  }

  Result<std::string_view> take_verbatim(std::string_view expected) {
    if (index_ >= end_) {
      return Error(ErrorCode::TruncatedInput, "the state document ends before its terminator");
    }
    const std::string_view line = lines_[index_];
    if (line != expected) {
      return Error(ErrorCode::MalformedRecord, "expected the exact line \"" + std::string(expected) + "\"");
    }
    ++index_;
    return line;
  }

  Result<std::vector<std::string_view>> take_fields(std::string_view key, std::size_t count) {
    FCR_TRY(line, take(key));
    FCR_TRY(fields, split_fields(line, '|', count + 1));
    if (fields.size() != count) {
      return Error(ErrorCode::CountMismatch,
                   "field \"" + std::string(key) + "\" must have exactly " + format_unsigned(count) + " parts");
    }
    return fields;
  }

 private:
  const std::vector<std::string_view>& lines_;
  std::size_t index_ = 0;
  std::size_t end_ = 0;
};

Result<std::uint64_t> nonzero(std::uint64_t value, std::string_view what) {
  if (value == 0) {
    return Error(ErrorCode::MissingField, std::string(what) + " must not be zero");
  }
  return value;
}

}  // namespace

bool looks_like_state(std::string_view document) noexcept {
  return document.size() > kStateSignature.size() &&
         document.substr(0, kStateSignature.size()) == kStateSignature;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

Result<std::string> encode_state(const ReservationLedger& ledger) {
  std::string body;
  body.reserve(4096);

  body.append(kStateSignature);
  body.push_back(' ');
  body.append(format_unsigned(kStateFormatVersion));
  body.push_back('\n');

  append_line(body, "incarnation", ledger.incarnation().to_string());
  append_number(body, "revision", ledger.revision().value());
  append_number(body, "last-tick", ledger.last_observed_tick().value());

  const CapacitySnapshot& capacity = ledger.capacity();
  append_number(body, "capacity", ledger.has_capacity() ? 1U : 0U);
  append_line(body, "snapshot-ref", present(capacity.ref.value()));
  append_number(body, "snapshot-source-generation", capacity.source_generation.value());
  append_line(body, "snapshot-facility", present(capacity.facility.value()));
  append_number(body, "snapshot-tick", capacity.captured_at_tick.value());
  append_line(body, "snapshot-digest", present(capacity.body_digest));

  if (ledger.has_capacity()) {
    append_number(body, "pools", static_cast<std::uint64_t>(capacity.pools.size()));
    for (const CapacityPool& pool : capacity.pools) {
      std::string line;
      line.append(resource_kind_token(pool.key.kind));
      line.push_back('|');
      line.append(pool.key.scope.value());
      line.push_back('|');
      line.append(format_unsigned(pool.gross));
      line.push_back('|');
      line.append(format_unsigned(pool.withdrawn));
      line.push_back('|');
      line.append(format_unsigned(pool.floor));
      append_line(body, "pool", line);
    }
  } else {
    append_number(body, "pools", 0U);
  }

  const auto& records = ledger.records_for_persistence();
  append_number(body, "reservations", static_cast<std::uint64_t>(records.size()));
  for (const auto& entry : records) {
    const ReservationRecord& record = entry.second;
    body.append("reservation.begin\n");
    append_line(body, "reservation.id", record.id.value());
    append_number(body, "reservation.generation", record.generation.value());
    append_number(body, "reservation.authority-epoch", record.authority_epoch.value());
    append_number(body, "reservation.revision", record.revision.value());
    append_line(body, "reservation.last-attempt", record.last_attempt.value());
    append_line(body, "reservation.claimant", record.claimant.value());
    append_line(body, "reservation.tenant", record.tenant.value());
    append_line(body, "reservation.service", record.service.value());
    append_line(body, "reservation.priority", record.priority.value());
    append_line(body, "reservation.headroom", headroom_class_token(record.headroom));
    append_line(body, "reservation.source-snapshot", present(record.source_snapshot.value()));
    append_number(body, "reservation.source-generation", record.source_generation.value());
    {
      std::string validity = format_unsigned(record.validity.start.value());
      validity.push_back('|');
      validity.append(format_unsigned(record.validity.deadline.value()));
      append_line(body, "reservation.validity", validity);
    }
    append_line(body, "reservation.state", reservation_state_token(record.state));
    append_number(body, "reservation.created-at-tick", record.created_at_tick.value());
    append_number(body, "reservation.updated-at-tick", record.updated_at_tick.value());

    append_number(body, "reservation.claims", static_cast<std::uint64_t>(record.claims.size()));
    for (const ResourceClaim& claim : record.claims) {
      std::string line;
      line.append(resource_kind_token(claim.pool.kind));
      line.push_back('|');
      line.append(claim.pool.scope.value());
      line.push_back('|');
      line.append(format_unsigned(claim.amount));
      append_line(body, "claim", line);
    }

    append_number(body, "reservation.lineage", static_cast<std::uint64_t>(record.lineage.size()));
    for (const AmendmentEntry& line : record.lineage) {
      FCR_TRY(encoded_detail, encode_field(line.detail, kMaxTextBytes));
      std::string text;
      text.append(format_unsigned(line.generation.value()));
      text.push_back('|');
      text.append(format_unsigned(line.predecessor.value()));
      text.push_back('|');
      text.append(line.attempt.value());
      text.push_back('|');
      text.append(format_unsigned(line.revision.value()));
      text.push_back('|');
      text.append(format_unsigned(line.epoch.value()));
      text.push_back('|');
      text.append(format_unsigned(line.at_tick.value()));
      text.push_back('|');
      text.append(amendment_cause_token(line.cause));
      text.push_back('|');
      text.append(encoded_detail);
      append_line(body, "lineage", text);
    }

    if (record.termination.has_value()) {
      const TerminationProvenance& provenance = *record.termination;
      FCR_TRY(encoded_detail, encode_field(provenance.detail, kMaxTextBytes));
      append_number(body, "reservation.termination", 1U);
      std::string text;
      text.append(transition_cause_token(provenance.cause));
      text.push_back('|');
      text.append(present(provenance.actor.value()));
      text.push_back('|');
      text.append(provenance.attempt.value());
      text.push_back('|');
      text.append(format_unsigned(provenance.revision.value()));
      text.push_back('|');
      text.append(format_unsigned(provenance.epoch.value()));
      text.push_back('|');
      text.append(format_unsigned(provenance.at_tick.value()));
      text.push_back('|');
      text.append(provenance.policy.has_value() ? present(provenance.policy->value()) : std::string(kAbsent));
      text.push_back('|');
      text.append(encoded_detail);
      append_line(body, "termination", text);
    } else {
      append_number(body, "reservation.termination", 0U);
    }
    body.append("reservation.end\n");
  }

  const auto& order = ledger.attempt_order_for_persistence();
  const auto& attempts = ledger.attempts_for_persistence();
  append_number(body, "attempts", static_cast<std::uint64_t>(attempts.size()));
  for (const AttemptId& id : order) {
    const auto position = attempts.find(id);
    if (position == attempts.end()) {
      return Error(ErrorCode::InvariantViolation, "the attempt eviction order names an unknown attempt")
          .with_subject(id.value());
    }
    const AttemptRecord& record = position->second;
    std::string text;
    text.append(id.value());
    text.push_back('|');
    text.append(operation_kind_token(record.operation));
    text.push_back('|');
    text.append(present(record.subject.value()));
    text.push_back('|');
    text.append(record.intent_digest);
    text.push_back('|');
    text.append(format_unsigned(record.revision.value()));
    text.push_back('|');
    text.append(format_unsigned(record.epoch.value()));
    text.push_back('|');
    text.append(format_unsigned(record.generation.value()));
    text.push_back('|');
    text.append(format_unsigned(record.previous_source_generation.value()));
    text.push_back('|');
    text.append(format_unsigned(record.at_tick.value()));
    append_line(body, "attempt", text);
  }

  body.append("end\n");

  std::string document = body;
  document.append(kDigestPrefix);
  document.append(sha256_hex(body));
  document.push_back('\n');
  return document;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

Result<ReservationLedger> decode_state(std::string_view document, const Incarnation& expected_incarnation,
                                       AuthorityEpoch epoch, LedgerOptions options, CapacityOrigin origin,
                                       const StateLimits& limits) {
  if (document.size() > limits.max_bytes) {
    return Error(ErrorCode::LimitExceeded, "the state document exceeds the configured maximum size");
  }
  FCR_TRY(lines, split_lines(document, limits.max_lines));
  if (lines.size() < 4) {
    return Error(ErrorCode::TruncatedInput, "the state document is too short");
  }

  const std::string_view digest_line = lines.back();
  if (digest_line.size() != kDigestPrefix.size() + Sha256::kDigestHexDigits ||
      digest_line.substr(0, kDigestPrefix.size()) != kDigestPrefix) {
    return Error(ErrorCode::MalformedRecord, "the state document does not end with an integrity digest");
  }
  const std::string_view declared = digest_line.substr(kDigestPrefix.size());
  if (!is_lower_hex(declared, Sha256::kDigestHexDigits)) {
    return Error(ErrorCode::MalformedRecord, "the state digest must be 64 lowercase hexadecimal digits");
  }
  const std::size_t body_bytes = document.size() - digest_line.size() - 1;
  if (sha256_hex(document.substr(0, body_bytes)) != declared) {
    return Error(ErrorCode::DigestMismatch, "the state document digest does not match its body");
  }

  Cursor cursor(lines, lines.size() - 1);

  {
    const std::string_view signature = lines[0];
    const std::string prefix = std::string(kStateSignature) + " ";
    if (signature.size() <= prefix.size() || signature.substr(0, prefix.size()) != prefix) {
      return Error(ErrorCode::MalformedRecord, "the state document does not start with a state signature");
    }
    FCR_TRY(version, parse_unsigned(signature.substr(prefix.size())));
    if (version != kStateFormatVersion) {
      return Error(ErrorCode::UnsupportedFormatVersion,
                   "state format version " + format_unsigned(version) + " is not supported");
    }
    FCR_TRYV(cursor.take_verbatim(signature));
  }

  FCR_TRY(incarnation_text, cursor.take("incarnation"));
  FCR_TRY(incarnation, Incarnation::parse(incarnation_text));
  if (incarnation != expected_incarnation) {
    return Error(ErrorCode::IncarnationMismatch,
                 "the state document belongs to a different store incarnation")
        .with_subject(incarnation.to_string());
  }

  FCR_TRY(revision_value, cursor.take_number("revision"));
  FCR_TRY(last_tick_value, cursor.take_number("last-tick"));

  FCR_TRY(capacity_flag, cursor.take_number("capacity"));
  if (capacity_flag > 1) {
    return Error(ErrorCode::MalformedRecord, "the capacity flag must be 0 or 1");
  }
  CapacitySnapshot capacity;
  FCR_TRY(ref_text, cursor.take("snapshot-ref"));
  FCR_TRY(source_text, cursor.take_number("snapshot-source-generation"));
  FCR_TRY(facility_text, cursor.take("snapshot-facility"));
  FCR_TRY(snapshot_tick, cursor.take_number("snapshot-tick"));
  FCR_TRY(snapshot_digest, cursor.take("snapshot-digest"));
  FCR_TRY(pool_count, cursor.take_number("pools"));
  if (pool_count > limits.max_pools) {
    return Error(ErrorCode::LimitExceeded, "the state declares more pools than the configured maximum");
  }

  const bool has_capacity = capacity_flag == 1;
  if (has_capacity) {
    if (ref_text == kAbsent || facility_text == kAbsent) {
      return Error(ErrorCode::MalformedRecord, "a state that holds capacity must name its snapshot and facility");
    }
    FCR_TRYV(nonzero(source_text, "the snapshot source generation"));
    FCR_TRYV(nonzero(snapshot_tick, "the snapshot capture tick"));
    FCR_TRY(ref_id, SnapshotRef::parse(ref_text, "snapshot ref"));
    capacity.ref = ref_id;
    capacity.source_generation = SourceGeneration(source_text);
    FCR_TRY(facility_id, FacilityRef::parse(facility_text, "facility ref"));
    capacity.facility = facility_id;
    capacity.captured_at_tick = Tick(snapshot_tick);
    if (snapshot_digest != kAbsent && !is_lower_hex(snapshot_digest, Sha256::kDigestHexDigits)) {
      return Error(ErrorCode::MalformedRecord, "the snapshot digest must be 64 lowercase hexadecimal digits");
    }
    capacity.body_digest = snapshot_digest == kAbsent ? std::string() : std::string(snapshot_digest);
    capacity.pools.reserve(static_cast<std::size_t>(pool_count));
    for (std::uint64_t ordinal = 0; ordinal < pool_count; ++ordinal) {
      FCR_TRY(fields, cursor.take_fields("pool", 5));
      CapacityPool pool;
      FCR_TRY(kind, parse_resource_kind(fields[0]));
      pool.key.kind = kind;
      FCR_TRY(scope, ScopeRef::parse(fields[1], "pool scope"));
      pool.key.scope = scope;
      FCR_TRY(gross, parse_unsigned(fields[2]));
      pool.gross = gross;
      FCR_TRY(withdrawn, parse_unsigned(fields[3]));
      pool.withdrawn = withdrawn;
      FCR_TRY(floor, parse_unsigned(fields[4]));
      pool.floor = floor;
      FCR_TRYV(pool.available());
      FCR_TRYV(pool.reservable());
      if (!capacity.pools.empty() && !(capacity.pools.back().key < pool.key)) {
        return Error(ErrorCode::NonCanonicalOrder, "the state's capacity pools are not in canonical order");
      }
      capacity.pools.push_back(pool);
    }
  } else {
    if (pool_count != 0) {
      return Error(ErrorCode::CountMismatch, "a state that holds no capacity must declare no pools");
    }
    capacity.ref = SnapshotRef();
    capacity.facility = FacilityRef();
  }

  FCR_TRY(reservation_count, cursor.take_number("reservations"));
  if (reservation_count > limits.max_reservations) {
    return Error(ErrorCode::LimitExceeded, "the state declares more reservations than the configured maximum");
  }

  std::map<ReservationId, ReservationRecord> records;
  for (std::uint64_t ordinal = 0; ordinal < reservation_count; ++ordinal) {
    FCR_TRYV(cursor.take_verbatim("reservation.begin"));
    ReservationRecord record;

    FCR_TRY(id_text, cursor.take("reservation.id"));
    FCR_TRY(id, ReservationId::parse(id_text, "reservation id"));
    record.id = id;
    FCR_TRY(generation, cursor.take_number("reservation.generation"));
    FCR_TRYV(nonzero(generation, "the reservation generation"));
    record.generation = ReservationGeneration(generation);
    FCR_TRY(record_epoch, cursor.take_number("reservation.authority-epoch"));
    record.authority_epoch = AuthorityEpoch(record_epoch);
    FCR_TRY(record_revision, cursor.take_number("reservation.revision"));
    record.revision = Revision(record_revision);
    FCR_TRY(attempt_text, cursor.take("reservation.last-attempt"));
    FCR_TRY(attempt_id, AttemptId::parse(attempt_text, "attempt id"));
    record.last_attempt = attempt_id;
    FCR_TRY(claimant_text, cursor.take("reservation.claimant"));
    FCR_TRY(claimant, ClaimantRef::parse(claimant_text, "claimant reference"));
    record.claimant = claimant;
    FCR_TRY(tenant_text, cursor.take("reservation.tenant"));
    FCR_TRY(tenant, TenantRef::parse(tenant_text, "tenant reference"));
    record.tenant = tenant;
    FCR_TRY(service_text, cursor.take("reservation.service"));
    FCR_TRY(service, ServiceRef::parse(service_text, "service reference"));
    record.service = service;
    FCR_TRY(priority_text, cursor.take("reservation.priority"));
    FCR_TRY(priority, PriorityRef::parse(priority_text, "priority reference"));
    record.priority = priority;
    FCR_TRY(headroom_text, cursor.take("reservation.headroom"));
    FCR_TRY(headroom, parse_headroom_class(headroom_text));
    record.headroom = headroom;
    FCR_TRY(source_snapshot_text, cursor.take("reservation.source-snapshot"));
    if (source_snapshot_text != kAbsent) {
      FCR_TRY(source_snapshot, SnapshotRef::parse(source_snapshot_text, "source snapshot reference"));
      record.source_snapshot = source_snapshot;
    }
    FCR_TRY(source_generation_value, cursor.take_number("reservation.source-generation"));
    record.source_generation = SourceGeneration(source_generation_value);
    FCR_TRY(validity_fields, cursor.take_fields("reservation.validity", 2));
    FCR_TRY(start, parse_unsigned(validity_fields[0]));
    FCR_TRY(deadline, parse_unsigned(validity_fields[1]));
    record.validity.start = Tick(start);
    record.validity.deadline = Tick(deadline);
    FCR_TRYV(record.validity.validate());
    FCR_TRY(state_text, cursor.take("reservation.state"));
    FCR_TRY(state, parse_reservation_state(state_text));
    record.state = state;
    FCR_TRY(created, cursor.take_number("reservation.created-at-tick"));
    FCR_TRYV(nonzero(created, "the reservation creation tick"));
    record.created_at_tick = Tick(created);
    FCR_TRY(updated, cursor.take_number("reservation.updated-at-tick"));
    FCR_TRYV(nonzero(updated, "the reservation update tick"));
    record.updated_at_tick = Tick(updated);

    FCR_TRY(claim_count, cursor.take_number("reservation.claims"));
    if (claim_count == 0 || claim_count > limits.max_pools_per_reservation) {
      return Error(ErrorCode::CountMismatch, "a reservation must declare between one and the maximum number of claims")
          .with_subject(record.id.value());
    }
    record.claims.reserve(static_cast<std::size_t>(claim_count));
    for (std::uint64_t claim_ordinal = 0; claim_ordinal < claim_count; ++claim_ordinal) {
      FCR_TRY(fields, cursor.take_fields("claim", 3));
      ResourceClaim claim;
      FCR_TRY(kind, parse_resource_kind(fields[0]));
      claim.pool.kind = kind;
      FCR_TRY(scope, ScopeRef::parse(fields[1], "claim scope"));
      claim.pool.scope = scope;
      FCR_TRY(amount, parse_unsigned(fields[2]));
      if (amount == 0) {
        return Error(ErrorCode::ClaimAmountZero, "a persisted claim must not be zero").with_subject(record.id.value());
      }
      claim.amount = amount;
      if (!record.claims.empty() && !(record.claims.back().pool < claim.pool)) {
        return Error(ErrorCode::NonCanonicalOrder, "a persisted claim set is not in canonical order")
            .with_subject(record.id.value());
      }
      record.claims.push_back(claim);
    }

    FCR_TRY(lineage_count, cursor.take_number("reservation.lineage"));
    if (lineage_count > limits.max_lineage_per_reservation) {
      return Error(ErrorCode::LimitExceeded, "the state declares a longer amendment lineage than the maximum")
          .with_subject(record.id.value());
    }
    record.lineage.reserve(static_cast<std::size_t>(lineage_count));
    for (std::uint64_t line_ordinal = 0; line_ordinal < lineage_count; ++line_ordinal) {
      FCR_TRY(fields, cursor.take_fields("lineage", 8));
      AmendmentEntry entry;
      FCR_TRY(generation_value, parse_unsigned(fields[0]));
      entry.generation = ReservationGeneration(generation_value);
      FCR_TRY(predecessor_value, parse_unsigned(fields[1]));
      entry.predecessor = ReservationGeneration(predecessor_value);
      FCR_TRY(attempt, AttemptId::parse(fields[2], "attempt id"));
      entry.attempt = attempt;
      FCR_TRY(entry_revision, parse_unsigned(fields[3]));
      entry.revision = Revision(entry_revision);
      FCR_TRY(entry_epoch, parse_unsigned(fields[4]));
      entry.epoch = AuthorityEpoch(entry_epoch);
      FCR_TRY(entry_tick, parse_unsigned(fields[5]));
      entry.at_tick = Tick(entry_tick);
      FCR_TRY(cause, parse_amendment_cause(fields[6]));
      entry.cause = cause;
      FCR_TRY(detail, decode_field(fields[7], kMaxTextBytes));
      entry.detail = std::move(detail);
      record.lineage.push_back(std::move(entry));
    }

    FCR_TRY(termination_flag, cursor.take_number("reservation.termination"));
    if (termination_flag > 1) {
      return Error(ErrorCode::MalformedRecord, "the termination flag must be 0 or 1");
    }
    if (termination_flag == 1) {
      FCR_TRY(fields, cursor.take_fields("termination", 8));
      TerminationProvenance provenance;
      FCR_TRY(cause, parse_transition_cause(fields[0]));
      provenance.cause = cause;
      if (fields[1] != kAbsent) {
        FCR_TRY(actor, ActorRef::parse(fields[1], "actor reference"));
        provenance.actor = actor;
      }
      FCR_TRY(attempt, AttemptId::parse(fields[2], "attempt id"));
      provenance.attempt = attempt;
      FCR_TRY(provenance_revision, parse_unsigned(fields[3]));
      provenance.revision = Revision(provenance_revision);
      FCR_TRY(provenance_epoch, parse_unsigned(fields[4]));
      provenance.epoch = AuthorityEpoch(provenance_epoch);
      FCR_TRY(provenance_tick, parse_unsigned(fields[5]));
      provenance.at_tick = Tick(provenance_tick);
      if (fields[6] != kAbsent) {
        FCR_TRY(policy, PolicyRef::parse(fields[6], "policy reference"));
        provenance.policy = policy;
      }
      FCR_TRY(detail, decode_field(fields[7], kMaxTextBytes));
      provenance.detail = std::move(detail);
      record.termination = std::move(provenance);
    }

    FCR_TRYV(cursor.take_verbatim("reservation.end"));

    if (record.id.empty()) {
      return Error(ErrorCode::MissingField, "a persisted reservation must carry an identity");
    }
    if (!records.empty() && !(records.rbegin()->first < record.id)) {
      return Error(ErrorCode::NonCanonicalOrder, "the state's reservations are not in canonical order")
          .with_subject(record.id.value());
    }
    if (!records.emplace(record.id, std::move(record)).second) {
      return Error(ErrorCode::DuplicateField, "the state holds two reservations with the same identity");
    }
  }

  FCR_TRY(attempt_count, cursor.take_number("attempts"));
  if (attempt_count > limits.max_attempts) {
    return Error(ErrorCode::LimitExceeded, "the state declares more attempts than the configured maximum");
  }
  std::map<AttemptId, AttemptRecord> attempts;
  std::deque<AttemptId> attempt_order;
  for (std::uint64_t ordinal = 0; ordinal < attempt_count; ++ordinal) {
    FCR_TRY(fields, cursor.take_fields("attempt", 9));
    FCR_TRY(id, AttemptId::parse(fields[0], "attempt id"));
    AttemptRecord record;
    FCR_TRY(operation, parse_operation_kind(fields[1]));
    record.operation = operation;
    if (fields[2] != kAbsent) {
      FCR_TRY(subject, ReservationId::parse(fields[2], "reservation id"));
      record.subject = subject;
    }
    if (!is_lower_hex(fields[3], Sha256::kDigestHexDigits)) {
      return Error(ErrorCode::MalformedRecord, "an attempt intent digest must be 64 lowercase hexadecimal digits");
    }
    record.intent_digest = std::string(fields[3]);
    FCR_TRY(attempt_revision, parse_unsigned(fields[4]));
    record.revision = Revision(attempt_revision);
    FCR_TRY(epoch_value, parse_unsigned(fields[5]));
    record.epoch = AuthorityEpoch(epoch_value);
    FCR_TRY(generation_value, parse_unsigned(fields[6]));
    record.generation = ReservationGeneration(generation_value);
    FCR_TRY(previous_value, parse_unsigned(fields[7]));
    record.previous_source_generation = SourceGeneration(previous_value);
    FCR_TRY(tick_value, parse_unsigned(fields[8]));
    record.at_tick = Tick(tick_value);
    if (!attempts.emplace(id, std::move(record)).second) {
      return Error(ErrorCode::DuplicateField, "the state holds two attempts with the same identity");
    }
    attempt_order.push_back(id);
  }

  FCR_TRYV(cursor.take_verbatim("end"));
  if (!cursor.at_end()) {
    return Error(ErrorCode::CountMismatch, "the state document holds lines after its terminator");
  }

  LedgerOptions effective = options;
  (void)effective;
  FCR_TRY(ledger, ReservationLedger::reassemble(std::move(incarnation), epoch, options, std::move(capacity),
                                                has_capacity, origin, Revision(revision_value), std::move(records),
                                                std::move(attempts), std::move(attempt_order)));
  // The logical tick the authority last acted on is part of the persisted state:
  // it is what makes a reopened store's "deadline passed" reporting agree with
  // the run that wrote the state.
  ledger.note_observed_tick(Tick(last_tick_value));
  return ledger;
}

}  // namespace dccp::facility_capacity_reservation::detail
