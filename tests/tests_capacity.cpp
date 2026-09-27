// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Capacity snapshot consumption: parsing, canonical writing, consistency and
// every rejection the untrusted-input path must make.

#include <algorithm>
#include <string>
#include <vector>

#include "dccp/facility_capacity_reservation/capacity.hpp"
#include "dccp/facility_capacity_reservation/digest.hpp"
#include "dccp/facility_capacity_reservation/text.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

fcr::CapacitySnapshot sample_snapshot() {
  return fcr_test::make_snapshot("snap-1", "site-1", 5, 1'000,
                                 {{fcr::ResourceKind::Rack, "hall-a", 40, 4, 6},
                                  {fcr::ResourceKind::Power, "feed-a", 2'000'000, 0, 100'000},
                                  {fcr::ResourceKind::Cooling, "loop-a", 1'800'000, 0, 0},
                                  {fcr::ResourceKind::Space, "hall-a", 400, 0, 0},
                                  {fcr::ResourceKind::FacilityService, "lifts", 10, 1, 2}});
}

/// Re-seals a document after its body has been edited, so that a test can reach
/// past the digest check and exercise the parser proper.
std::string reseal(std::string body) {
  // The digest covers the body only, so it is computed before the digest line is
  // appended; hashing after appending would include the prefix and produce a
  // document the parser correctly refuses.
  const std::string digest = fcr::sha256_hex(body);
  body.append("digest=sha256:");
  body.append(digest);
  body.push_back('\n');
  return body;
}

std::string body_of(const std::string& document) {
  const std::size_t position = document.rfind("digest=sha256:");
  return document.substr(0, position);
}

}  // namespace

FCR_TEST(capacity, kinds_have_stable_canonical_tokens) {
  FCR_CHECK_EQ(std::string(fcr::resource_kind_token(fcr::ResourceKind::Space)), std::string("space"));
  FCR_CHECK_EQ(std::string(fcr::resource_kind_token(fcr::ResourceKind::Rack)), std::string("rack"));
  FCR_CHECK_EQ(std::string(fcr::resource_kind_token(fcr::ResourceKind::Power)), std::string("power"));
  FCR_CHECK_EQ(std::string(fcr::resource_kind_token(fcr::ResourceKind::Cooling)), std::string("cooling"));
  FCR_CHECK_EQ(std::string(fcr::resource_kind_token(fcr::ResourceKind::FacilityService)),
               std::string("facility-service"));
  for (const fcr::ResourceKind kind : {fcr::ResourceKind::Space, fcr::ResourceKind::Rack, fcr::ResourceKind::Power,
                                       fcr::ResourceKind::Cooling, fcr::ResourceKind::FacilityService}) {
    FCR_CHECK_EQ(fcr::parse_resource_kind(fcr::resource_kind_token(kind)).value(), kind);
  }
}

FCR_TEST(capacity, there_is_no_accelerator_or_network_kind) {
  // The boundary is enforced by the vocabulary itself: a facility capacity
  // reservation cannot name accelerator compute, accelerator memory or network
  // bandwidth, because no such kind exists.
  FCR_CHECK_ERROR(fcr::parse_resource_kind("gpu"), fcr::ErrorCode::UnknownEnumToken);
  FCR_CHECK_ERROR(fcr::parse_resource_kind("accelerator-memory"), fcr::ErrorCode::UnknownEnumToken);
  FCR_CHECK_ERROR(fcr::parse_resource_kind("bandwidth"), fcr::ErrorCode::UnknownEnumToken);
  FCR_CHECK_ERROR(fcr::parse_resource_kind(""), fcr::ErrorCode::UnknownEnumToken);
  FCR_CHECK_ERROR(fcr::parse_resource_kind("RACK"), fcr::ErrorCode::UnknownEnumToken);
}

FCR_TEST(capacity, pool_derives_available_and_reservable_exactly) {
  fcr::CapacityPool pool;
  pool.key.kind = fcr::ResourceKind::Rack;
  pool.key.scope = fcr_test::scope("hall-a");
  pool.gross = 100;
  pool.withdrawn = 10;
  pool.floor = 5;
  FCR_CHECK_EQ(pool.available().value(), 90ULL);
  FCR_CHECK_EQ(pool.reservable().value(), 85ULL);
}

FCR_TEST(capacity, pool_reports_inconsistency_instead_of_wrapping) {
  fcr::CapacityPool pool;
  pool.key.kind = fcr::ResourceKind::Rack;
  pool.key.scope = fcr_test::scope("hall-a");
  pool.gross = 10;
  pool.withdrawn = 11;
  FCR_CHECK_ERROR(pool.available(), fcr::ErrorCode::SnapshotInconsistent);
  FCR_CHECK_ERROR(pool.reservable(), fcr::ErrorCode::SnapshotInconsistent);

  fcr::CapacityPool floored;
  floored.key.kind = fcr::ResourceKind::Rack;
  floored.key.scope = fcr_test::scope("hall-a");
  floored.gross = 10;
  floored.withdrawn = 4;
  floored.floor = 7;
  FCR_CHECK_EQ(floored.available().value(), 6ULL);
  FCR_CHECK_ERROR(floored.reservable(), fcr::ErrorCode::SnapshotInconsistent);
}

FCR_TEST(capacity, write_then_parse_round_trips_exactly) {
  const fcr::CapacitySnapshot original = sample_snapshot();
  const fcr::Result<std::string> document = fcr::write_capacity_snapshot(original);
  FCR_REQUIRE(document.has_value());
  const fcr::Result<fcr::CapacitySnapshot> parsed =
      fcr::parse_capacity_snapshot(*document, fcr::SnapshotLimits{});
  FCR_REQUIRE(parsed.has_value());
  FCR_CHECK_EQ(parsed->ref.value(), original.ref.value());
  FCR_CHECK_EQ(parsed->facility.value(), original.facility.value());
  FCR_CHECK_EQ(parsed->source_generation.value(), original.source_generation.value());
  FCR_CHECK_EQ(parsed->captured_at_tick.value(), original.captured_at_tick.value());
  FCR_REQUIRE(parsed->pools.size() == original.pools.size());
  for (std::size_t index = 0; index < original.pools.size(); ++index) {
    FCR_CHECK(parsed->pools[index].key == original.pools[index].key);
    FCR_CHECK_EQ(parsed->pools[index].gross, original.pools[index].gross);
    FCR_CHECK_EQ(parsed->pools[index].withdrawn, original.pools[index].withdrawn);
    FCR_CHECK_EQ(parsed->pools[index].floor, original.pools[index].floor);
  }
  // Writing the parsed form again is byte-identical: the format is canonical.
  const fcr::Result<std::string> again = fcr::write_capacity_snapshot(*parsed);
  FCR_REQUIRE(again.has_value());
  FCR_CHECK_EQ(*again, *document);
  FCR_CHECK_EQ(parsed->body_digest, fcr::sha256_hex(body_of(*document)));
}

FCR_TEST(capacity, find_and_reservable_of_kind_are_exact) {
  const fcr::CapacitySnapshot snapshot = sample_snapshot();
  const fcr::PoolKey key{fcr::ResourceKind::Rack, fcr_test::scope("hall-a")};
  FCR_REQUIRE(snapshot.find(key) != nullptr);
  FCR_CHECK_EQ(snapshot.find(key)->reservable().value(), 30ULL);
  const fcr::PoolKey missing{fcr::ResourceKind::Rack, fcr_test::scope("hall-z")};
  FCR_CHECK(snapshot.find(missing) == nullptr);
  FCR_CHECK_EQ(snapshot.reservable_of_kind(fcr::ResourceKind::Rack).value(), 30ULL);
  FCR_CHECK_EQ(snapshot.reservable_of_kind(fcr::ResourceKind::Power).value(), 1'900'000ULL);
  FCR_CHECK_EQ(snapshot.reservable_of_kind(fcr::ResourceKind::FacilityService).value(), 7ULL);
}

FCR_TEST(capacity, parser_rejects_a_tampered_body) {
  const fcr::Result<std::string> document = fcr::write_capacity_snapshot(sample_snapshot());
  FCR_REQUIRE(document.has_value());
  std::string tampered = *document;
  const std::size_t position = tampered.find("pool=rack|hall-a|40");
  FCR_REQUIRE(position != std::string::npos);
  tampered.replace(position, std::string("pool=rack|hall-a|40").size(), "pool=rack|hall-a|41");
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(tampered, fcr::SnapshotLimits{}), fcr::ErrorCode::DigestMismatch);
}

FCR_TEST(capacity, parser_rejects_truncation_and_missing_terminator) {
  const fcr::Result<std::string> document = fcr::write_capacity_snapshot(sample_snapshot());
  FCR_REQUIRE(document.has_value());
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(document->substr(0, document->size() / 2), fcr::SnapshotLimits{}),
                  fcr::ErrorCode::TruncatedInput);
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(document->substr(0, document->size() - 1), fcr::SnapshotLimits{}),
                  fcr::ErrorCode::TruncatedInput);
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot("", fcr::SnapshotLimits{}), fcr::ErrorCode::TruncatedInput);
}

FCR_TEST(capacity, parser_rejects_a_wrong_format_version) {
  std::string body = body_of(fcr::write_capacity_snapshot(sample_snapshot()).value());
  fcr_test::patch_text(body, "fcr-capacity-snapshot 1", "fcr-capacity-snapshot 9");
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(reseal(body), fcr::SnapshotLimits{}),
                  fcr::ErrorCode::UnsupportedFormatVersion);
}

FCR_TEST(capacity, parser_rejects_a_miscounted_pool_block) {
  std::string body = body_of(fcr::write_capacity_snapshot(sample_snapshot()).value());
  fcr_test::patch_text(body, "pools=5", "pools=4");
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(reseal(body), fcr::SnapshotLimits{}), fcr::ErrorCode::CountMismatch);
}

FCR_TEST(capacity, parser_rejects_unsorted_or_duplicate_pools) {
  const fcr::CapacitySnapshot snapshot = sample_snapshot();
  fcr::CapacitySnapshot unsorted = snapshot;
  std::reverse(unsorted.pools.begin(), unsorted.pools.end());
  FCR_CHECK_ERROR(fcr::write_capacity_snapshot(unsorted), fcr::ErrorCode::NonCanonicalOrder);

  fcr::CapacitySnapshot duplicated = snapshot;
  duplicated.pools.push_back(duplicated.pools.front());
  FCR_CHECK_ERROR(fcr::write_capacity_snapshot(duplicated), fcr::ErrorCode::NonCanonicalOrder);
}

FCR_TEST(capacity, parser_rejects_a_pool_with_the_wrong_field_count) {
  std::string body = body_of(fcr::write_capacity_snapshot(sample_snapshot()).value());
  fcr_test::patch_text(body, "pool=rack|hall-a|40|4|6", "pool=rack|hall-a|40|4");
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(reseal(body), fcr::SnapshotLimits{}), fcr::ErrorCode::CountMismatch);
}

FCR_TEST(capacity, parser_rejects_a_non_canonical_integer) {
  std::string body = body_of(fcr::write_capacity_snapshot(sample_snapshot()).value());
  fcr_test::patch_text(body, "pool=rack|hall-a|40|4|6", "pool=rack|hall-a|040|4|6");
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(reseal(body), fcr::SnapshotLimits{}), fcr::ErrorCode::MalformedRecord);
}

FCR_TEST(capacity, parser_rejects_an_unknown_kind_and_a_bad_scope) {
  const std::string original = body_of(fcr::write_capacity_snapshot(sample_snapshot()).value());
  std::string unknown_kind = original;
  fcr_test::patch_text(unknown_kind, "pool=rack|hall-a|", "pool=gpu|hall-a|");
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(reseal(unknown_kind), fcr::SnapshotLimits{}),
                  fcr::ErrorCode::UnknownEnumToken);

  std::string bad_scope = original;
  fcr_test::patch_text(bad_scope, "pool=rack|hall-a|", "pool=rack|../etc|");
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(reseal(bad_scope), fcr::SnapshotLimits{}),
                  fcr::ErrorCode::MalformedIdentifier);
}

FCR_TEST(capacity, parser_rejects_an_inconsistent_pool) {
  std::string withdrawn = body_of(fcr::write_capacity_snapshot(sample_snapshot()).value());
  fcr_test::patch_text(withdrawn, "pool=rack|hall-a|40|4|6", "pool=rack|hall-a|40|41|6");
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(reseal(withdrawn), fcr::SnapshotLimits{}),
                  fcr::ErrorCode::SnapshotInconsistent);

  std::string floor = body_of(fcr::write_capacity_snapshot(sample_snapshot()).value());
  fcr_test::patch_text(floor, "pool=rack|hall-a|40|4|6", "pool=rack|hall-a|40|4|99");
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(reseal(floor), fcr::SnapshotLimits{}),
                  fcr::ErrorCode::SnapshotInconsistent);
}

FCR_TEST(capacity, parser_rejects_a_zero_source_generation_and_a_zero_tick) {
  const std::string original = body_of(fcr::write_capacity_snapshot(sample_snapshot()).value());
  std::string zero_generation = original;
  fcr_test::patch_text(zero_generation, "source-generation=5", "source-generation=0");
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(reseal(zero_generation), fcr::SnapshotLimits{}),
                  fcr::ErrorCode::MissingField);

  std::string zero_tick = original;
  fcr_test::patch_text(zero_tick, "captured-at-tick=1000", "captured-at-tick=0");
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(reseal(zero_tick), fcr::SnapshotLimits{}),
                  fcr::ErrorCode::MissingField);
}

FCR_TEST(capacity, parser_rejects_an_empty_and_an_oversized_snapshot) {
  std::string body = "fcr-capacity-snapshot 1\nref=snap-1\nsource-generation=5\nfacility=site-1\n"
                     "captured-at-tick=1000\npools=0\nend\n";
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(reseal(body), fcr::SnapshotLimits{}),
                  fcr::ErrorCode::SnapshotEmpty);

  fcr::CapacitySnapshot snapshot = sample_snapshot();
  snapshot.pools.clear();
  FCR_CHECK_ERROR(fcr::write_capacity_snapshot(snapshot), fcr::ErrorCode::SnapshotEmpty);

  fcr::SnapshotLimits limits;
  limits.max_pools = 2;
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(fcr::write_capacity_snapshot(sample_snapshot()).value(), limits),
                  fcr::ErrorCode::LimitExceeded);
}

FCR_TEST(capacity, writer_requires_the_identifying_fields) {
  fcr::CapacitySnapshot snapshot = sample_snapshot();
  snapshot.ref = fcr::SnapshotRef();
  FCR_CHECK_ERROR(fcr::write_capacity_snapshot(snapshot), fcr::ErrorCode::MissingField);

  fcr::CapacitySnapshot no_facility = sample_snapshot();
  no_facility.facility = fcr::FacilityRef();
  FCR_CHECK_ERROR(fcr::write_capacity_snapshot(no_facility), fcr::ErrorCode::MissingField);

  fcr::CapacitySnapshot no_generation = sample_snapshot();
  no_generation.source_generation = fcr::SourceGeneration(0);
  FCR_CHECK_ERROR(fcr::write_capacity_snapshot(no_generation), fcr::ErrorCode::MissingField);

  fcr::CapacitySnapshot no_tick = sample_snapshot();
  no_tick.captured_at_tick = fcr::Tick(0);
  FCR_CHECK_ERROR(fcr::write_capacity_snapshot(no_tick), fcr::ErrorCode::MissingField);
}

FCR_TEST(capacity, parser_rejects_trailing_content_after_the_terminator) {
  std::string body = body_of(fcr::write_capacity_snapshot(sample_snapshot()).value());
  body.append("extra=1\n");
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(reseal(body), fcr::SnapshotLimits{}), fcr::ErrorCode::CountMismatch);
}

FCR_TEST(capacity, parser_rejects_a_missing_field) {
  const std::string original = body_of(fcr::write_capacity_snapshot(sample_snapshot()).value());
  std::string no_facility;
  std::size_t position = 0;
  while (position < original.size()) {
    const std::size_t end = original.find('\n', position);
    const std::string line = original.substr(position, end - position);
    if (line.rfind("facility=", 0) != 0) {
      no_facility.append(line);
      no_facility.push_back('\n');
    }
    position = end + 1;
  }
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(reseal(no_facility), fcr::SnapshotLimits{}),
                  fcr::ErrorCode::MalformedRecord);
}

FCR_TEST(capacity, parser_rejects_oversized_documents_before_allocating) {
  fcr::SnapshotLimits limits;
  limits.max_bytes = 32;
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(fcr::write_capacity_snapshot(sample_snapshot()).value(), limits),
                  fcr::ErrorCode::LimitExceeded);
}
