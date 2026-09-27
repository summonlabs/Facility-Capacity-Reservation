// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity_reservation/capacity.hpp"

#include <algorithm>

#include "dccp/facility_capacity_reservation/digest.hpp"
#include "dccp/facility_capacity_reservation/text.hpp"

namespace dccp::facility_capacity_reservation {
namespace {

constexpr std::string_view kSnapshotHeader = "fcr-capacity-snapshot";
constexpr std::string_view kDigestPrefix = "digest=sha256:";
constexpr std::string_view kPoolPrefix = "pool=";
constexpr std::size_t kPoolFieldCount = 5;
constexpr std::size_t kFixedSnapshotLines = 6;  // signature, ref, source, facility, tick, count

Result<void> require_prefix(std::string_view line, std::string_view prefix) {
  if (line.size() < prefix.size() || line.substr(0, prefix.size()) != prefix) {
    return Error(ErrorCode::MalformedRecord, "expected field \"" + std::string(prefix) + "\"");
  }
  return ok();
}

/// Reads "prefix<value>" and returns the value.
Result<std::string_view> field_value(std::string_view line, std::string_view prefix) {
  FCR_TRYV(require_prefix(line, prefix));
  return line.substr(prefix.size());
}

Result<Units> parse_units(std::string_view text, std::string_view what) {
  FCR_TRY(value, parse_unsigned(text));
  (void)what;
  return value;
}

}  // namespace

std::string_view resource_kind_token(ResourceKind kind) noexcept {
  switch (kind) {
    case ResourceKind::Space:
      return "space";
    case ResourceKind::Rack:
      return "rack";
    case ResourceKind::Power:
      return "power";
    case ResourceKind::Cooling:
      return "cooling";
    case ResourceKind::FacilityService:
      return "facility-service";
  }
  return "unknown";
}

Result<ResourceKind> parse_resource_kind(std::string_view token) {
  if (token == "space") {
    return ResourceKind::Space;
  }
  if (token == "rack") {
    return ResourceKind::Rack;
  }
  if (token == "power") {
    return ResourceKind::Power;
  }
  if (token == "cooling") {
    return ResourceKind::Cooling;
  }
  if (token == "facility-service") {
    return ResourceKind::FacilityService;
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown resource kind token").with_subject(sanitize_for_display(token));
}

std::string_view resource_kind_name(ResourceKind kind) noexcept {
  switch (kind) {
    case ResourceKind::Space:
      return "space";
    case ResourceKind::Rack:
      return "rack";
    case ResourceKind::Power:
      return "power";
    case ResourceKind::Cooling:
      return "cooling";
    case ResourceKind::FacilityService:
      return "facility service";
  }
  return "unknown";
}

std::string_view resource_kind_unit(ResourceKind kind) noexcept {
  switch (kind) {
    case ResourceKind::Space:
      return "space-units";
    case ResourceKind::Rack:
      return "rack-units";
    case ResourceKind::Power:
      return "watts";
    case ResourceKind::Cooling:
      return "watts";
    case ResourceKind::FacilityService:
      return "service-units";
  }
  return "units";
}

const CapacityPool* CapacitySnapshot::find(const PoolKey& key) const {
  const auto position =
      std::lower_bound(pools.begin(), pools.end(), key,
                       [](const CapacityPool& pool, const PoolKey& wanted) { return pool.key < wanted; });
  if (position == pools.end() || position->key != key) {
    return nullptr;
  }
  return &*position;
}

Result<Units> CapacitySnapshot::reservable_of_kind(ResourceKind kind) const {
  Units total = 0;
  for (const CapacityPool& pool : pools) {
    if (pool.key.kind != kind) {
      continue;
    }
    FCR_TRY(units, pool.reservable());
    FCR_TRYV(checked_accumulate(total, units));
  }
  return total;
}

Result<CapacitySnapshot> parse_capacity_snapshot(std::string_view document, const SnapshotLimits& limits) {
  if (document.size() > limits.max_bytes) {
    return Error(ErrorCode::LimitExceeded, "capacity snapshot document exceeds the configured maximum size");
  }
  FCR_TRY(lines, split_lines(document, limits.max_lines));
  if (lines.size() < kFixedSnapshotLines + 2) {
    return Error(ErrorCode::TruncatedInput, "capacity snapshot document is too short");
  }

  // The final line is the integrity digest over every preceding byte.
  const std::string_view digest_line = lines.back();
  FCR_TRYV(require_prefix(digest_line, kDigestPrefix));
  const std::string_view declared_digest = digest_line.substr(kDigestPrefix.size());
  if (!is_lower_hex(declared_digest, Sha256::kDigestHexDigits)) {
    return Error(ErrorCode::MalformedRecord, "snapshot digest must be 64 lowercase hexadecimal digits");
  }
  const std::size_t body_bytes = document.size() - digest_line.size() - 1;
  const std::string computed_digest = sha256_hex(document.substr(0, body_bytes));
  if (computed_digest != declared_digest) {
    return Error(ErrorCode::DigestMismatch, "capacity snapshot digest does not match its body");
  }

  std::size_t index = 0;

  FCR_TRYV(require_prefix(lines[index], kSnapshotHeader));
  const std::string_view version_text = lines[index].substr(kSnapshotHeader.size());
  if (version_text.size() < 2 || version_text.front() != ' ') {
    return Error(ErrorCode::MalformedRecord, "snapshot signature must be followed by a format version");
  }
  FCR_TRY(version, parse_unsigned(version_text.substr(1)));
  if (version != kSnapshotFormatVersion) {
    return Error(ErrorCode::UnsupportedFormatVersion,
                 "capacity snapshot format version " + format_unsigned(version) + " is not supported");
  }
  ++index;

  CapacitySnapshot snapshot;
  FCR_TRY(ref_text, field_value(lines[index], "ref="));
  FCR_TRY(ref_id, SnapshotRef::parse(ref_text, "snapshot ref"));
  snapshot.ref = ref_id;
  ++index;

  FCR_TRY(source_text, field_value(lines[index], "source-generation="));
  FCR_TRY(source_value, parse_units(source_text, "source generation"));
  if (source_value == 0) {
    return Error(ErrorCode::MissingField, "source generation must not be zero");
  }
  snapshot.source_generation = SourceGeneration(source_value);
  ++index;

  FCR_TRY(facility_text, field_value(lines[index], "facility="));
  FCR_TRY(facility_id, FacilityRef::parse(facility_text, "facility ref"));
  snapshot.facility = facility_id;
  ++index;

  FCR_TRY(captured_text, field_value(lines[index], "captured-at-tick="));
  FCR_TRY(captured_value, parse_units(captured_text, "captured-at-tick"));
  if (captured_value == 0) {
    return Error(ErrorCode::MissingField, "captured-at-tick must not be zero");
  }
  snapshot.captured_at_tick = Tick(captured_value);
  ++index;

  FCR_TRY(count_text, field_value(lines[index], "pools="));
  FCR_TRY(count_value, parse_units(count_text, "pool count"));
  if (count_value > limits.max_pools) {
    return Error(ErrorCode::LimitExceeded, "capacity snapshot declares more pools than the configured maximum");
  }
  if (count_value == 0) {
    return Error(ErrorCode::SnapshotEmpty, "capacity snapshot declares no pools");
  }
  const std::size_t declared_pools = static_cast<std::size_t>(count_value);
  ++index;

  // Framing: fixed header lines, exactly `declared_pools` pool lines, the
  // terminator and the digest line. Nothing may be appended or omitted.
  const std::size_t expected_lines = kFixedSnapshotLines + declared_pools + 2;
  if (lines.size() != expected_lines) {
    return Error(ErrorCode::CountMismatch, "capacity snapshot declares " + format_unsigned(count_value) +
                                               " pools but frames " +
                                               format_unsigned(static_cast<std::uint64_t>(lines.size())));
  }

  snapshot.pools.reserve(declared_pools);
  for (std::size_t ordinal = 0; ordinal < declared_pools; ++ordinal) {
    const std::string_view line = lines[index + ordinal];
    FCR_TRYV(require_prefix(line, kPoolPrefix));
    FCR_TRY(fields, split_fields(line.substr(kPoolPrefix.size()), '|', kPoolFieldCount + 1));
    if (fields.size() != kPoolFieldCount) {
      return Error(ErrorCode::CountMismatch, "a pool record must have exactly five fields");
    }
    CapacityPool pool;
    FCR_TRY(kind, parse_resource_kind(fields[0]));
    pool.key.kind = kind;
    FCR_TRY(scope, ScopeRef::parse(fields[1], "pool scope"));
    pool.key.scope = scope;
    FCR_TRY(gross, parse_units(fields[2], "pool gross capacity"));
    pool.gross = gross;
    FCR_TRY(withdrawn, parse_units(fields[3], "pool withdrawn capacity"));
    pool.withdrawn = withdrawn;
    FCR_TRY(floor, parse_units(fields[4], "pool floor capacity"));
    pool.floor = floor;

    // A pool must be internally consistent before it is admitted: withdrawn
    // capacity may not exceed gross capacity, and the floor may not exceed what
    // remains.
    FCR_TRYV(pool.available());
    FCR_TRYV(pool.reservable());

    if (!snapshot.pools.empty() && !(snapshot.pools.back().key < pool.key)) {
      return Error(ErrorCode::NonCanonicalOrder, "pool records must be unique and in canonical order")
          .with_subject(pool.key.to_string());
    }
    snapshot.pools.push_back(pool);
  }
  index += declared_pools;

  if (lines[index] != "end") {
    return Error(ErrorCode::MalformedRecord, "capacity snapshot terminator must be exactly \"end\"");
  }

  snapshot.body_digest = computed_digest;
  return snapshot;
}

Result<std::string> write_capacity_snapshot(const CapacitySnapshot& snapshot) {
  if (snapshot.pools.empty()) {
    return Error(ErrorCode::SnapshotEmpty, "a capacity snapshot must declare at least one pool");
  }
  if (snapshot.ref.empty() || snapshot.facility.empty() || snapshot.source_generation.is_zero() ||
      snapshot.captured_at_tick.is_zero()) {
    return Error(ErrorCode::MissingField,
                 "a capacity snapshot requires a ref, a facility, a source generation and a capture tick");
  }
  for (std::size_t index = 0; index < snapshot.pools.size(); ++index) {
    const CapacityPool& pool = snapshot.pools[index];
    if (index > 0 && !(snapshot.pools[index - 1].key < pool.key)) {
      return Error(ErrorCode::NonCanonicalOrder, "pool records must be unique and in canonical order")
          .with_subject(pool.key.to_string());
    }
    FCR_TRYV(pool.available());
    FCR_TRYV(pool.reservable());
  }

  std::string body;
  body.reserve(256 + snapshot.pools.size() * 64);
  body.append(kSnapshotHeader);
  body.push_back(' ');
  body.append(format_unsigned(kSnapshotFormatVersion));
  body.push_back('\n');
  body.append("ref=");
  body.append(snapshot.ref.value());
  body.push_back('\n');
  body.append("source-generation=");
  body.append(format_unsigned(snapshot.source_generation.value()));
  body.push_back('\n');
  body.append("facility=");
  body.append(snapshot.facility.value());
  body.push_back('\n');
  body.append("captured-at-tick=");
  body.append(format_unsigned(snapshot.captured_at_tick.value()));
  body.push_back('\n');
  body.append("pools=");
  body.append(format_unsigned(static_cast<std::uint64_t>(snapshot.pools.size())));
  body.push_back('\n');
  for (const CapacityPool& pool : snapshot.pools) {
    body.append(kPoolPrefix);
    body.append(resource_kind_token(pool.key.kind));
    body.push_back('|');
    body.append(pool.key.scope.value());
    body.push_back('|');
    body.append(format_unsigned(pool.gross));
    body.push_back('|');
    body.append(format_unsigned(pool.withdrawn));
    body.push_back('|');
    body.append(format_unsigned(pool.floor));
    body.push_back('\n');
  }
  body.append("end\n");

  std::string document = body;
  document.append(kDigestPrefix);
  document.append(sha256_hex(body));
  document.push_back('\n');
  return document;
}

}  // namespace dccp::facility_capacity_reservation
