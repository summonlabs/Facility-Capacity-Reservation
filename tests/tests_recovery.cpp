// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Recovery: what a real close/reopen and a real process restart do to the
// authoritative state, and what the store refuses to guess at.
//
// The multiprocess scenarios live in tests_multiprocess.cpp; this suite covers
// the single-process reopen path and the recovery decisions that can be made
// without starting a second process.

#include <algorithm>
#include <string>
#include <vector>

#include "dccp/facility_capacity_reservation/digest.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

const std::vector<fcr_test::PoolSpec> kPools = {
    {fcr::ResourceKind::Rack, "hall-a", 40, 0, 0},
    {fcr::ResourceKind::Power, "feed-a", 400'000, 0, 0},
};

fcr::CapacitySnapshot snapshot_of(std::uint64_t generation) {
  return fcr_test::make_snapshot("snap-recovery", "site-recovery", generation, generation * 1'000, kPools);
}

/// A digest of the whole authoritative state as an observer sees it.
std::string fingerprint(const fcr::Store& store) {
  std::string material;
  const fcr::Result<std::vector<fcr::ReservationView>> views = store.list();
  if (!views.has_value()) {
    return "error";
  }
  for (const fcr::ReservationView& view : *views) {
    material.append(view.record.id.value());
    material.push_back('|');
    material.append(fcr::format_unsigned(view.record.generation.value()));
    material.push_back('|');
    material.append(fcr::reservation_state_token(view.record.state));
    material.push_back('|');
    for (const fcr::ResourceClaim& claim : view.record.claims) {
      material.append(claim.pool.to_string());
      material.push_back(':');
      material.append(fcr::format_unsigned(claim.amount));
      material.push_back(',');
    }
    material.push_back(';');
  }
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = store.pools();
  if (pools.has_value()) {
    for (const fcr::PoolAccount& account : *pools) {
      material.append(account.pool.to_string());
      material.push_back(':');
      material.append(fcr::format_unsigned(account.committed));
      material.push_back(':');
      material.append(fcr::format_unsigned(account.protected_));
      material.push_back(':');
      material.append(fcr::format_unsigned(account.free));
      material.push_back(';');
    }
  }
  return fcr::sha256_hex(material);
}

void build_populated(const std::filesystem::path& root) {
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  FCR_REQUIRE(created.has_value());
  FCR_REQUIRE_OK(created->install_capacity(snapshot_of(1), fcr::Tick(1'000)));
  fcr_test::Fixture fixture;
  fixture.snapshot = snapshot_of(1);
  fixture.epoch = created->epoch().value();
  const fcr::Result<fcr::ReserveOutcome> reserved = created->reserve(fcr_test::reserve_request(
      fixture, "res-a", "attempt-1",
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 6), fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 60'000)},
      1'100, 900'000));
  FCR_REQUIRE(reserved.has_value());
  FCR_REQUIRE_OK(created->amend(fcr_test::amend_request(
      fixture, "res-a", "attempt-2", reserved->reservation.record.generation,
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 9), fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 90'000)},
      1'100, 950'000)));
  created->close();
}

}  // namespace

FCR_TEST(recovery, a_reopened_store_holds_one_whole_authoritative_state) {
  fcr_test::TempDir directory("recovery_whole");
  const std::filesystem::path root = directory / "store";
  build_populated(root);

  std::string expected;
  {
    fcr::Result<fcr::Store> opened = fcr::Store::open(root);
    FCR_REQUIRE(opened.has_value());
    expected = fingerprint(*opened);
    opened->close();
  }
  for (int attempt = 0; attempt < 3; ++attempt) {
    fcr::Result<fcr::Store> reopened = fcr::Store::open(root);
    FCR_REQUIRE(reopened.has_value());
    FCR_CHECK_EQ(fingerprint(*reopened), expected);
    const fcr::Result<fcr::VerificationReport> report = reopened->verify();
    FCR_REQUIRE(report.has_value());
    FCR_CHECK(report->ok);
    reopened->close();
  }
}

FCR_TEST(recovery, a_missing_head_pointer_is_refused_rather_than_rebuilt) {
  fcr_test::TempDir directory("recovery_nohead");
  const std::filesystem::path root = directory / "store";
  build_populated(root);
  std::error_code error;
  std::filesystem::remove(root / "fcr.head", error);
  FCR_REQUIRE(!error);

  // The generation file is present and internally valid, but promoting it would
  // publish a state whose commit was never acknowledged. The store refuses.
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::HeadMissing);
  FCR_CHECK_ERROR(fcr::Store::create(root), fcr::ErrorCode::StoreNotEmpty);
}

FCR_TEST(recovery, a_head_pointer_naming_a_missing_generation_is_refused) {
  fcr_test::TempDir directory("recovery_missing");
  const std::filesystem::path root = directory / "store";
  build_populated(root);
  const std::filesystem::path committed = fcr_test::committed_state_file(root);
  std::error_code error;
  std::filesystem::remove(committed, error);
  FCR_REQUIRE(!error);
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::StoreCorrupt);
}

FCR_TEST(recovery, a_truncated_generation_is_refused) {
  fcr_test::TempDir directory("recovery_truncated");
  const std::filesystem::path root = directory / "store";
  build_populated(root);
  const std::filesystem::path committed = fcr_test::committed_state_file(root);
  const std::uintmax_t size = std::filesystem::file_size(committed);
  fcr_test::truncate_file(committed, size / 2);
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::IntegrityFailure);
}

FCR_TEST(recovery, a_generation_whose_bytes_were_changed_is_refused) {
  fcr_test::TempDir directory("recovery_changed");
  const std::filesystem::path root = directory / "store";
  build_populated(root);
  const std::filesystem::path committed = fcr_test::committed_state_file(root);
  fcr_test::patch_file(committed, "reservation.id=res-a", "reservation.id=res-b");
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::IntegrityFailure);
}

FCR_TEST(recovery, a_head_pointer_from_another_store_is_refused) {
  fcr_test::TempDir directory("recovery_swapped");
  const std::filesystem::path first = directory / "first";
  const std::filesystem::path second = directory / "second";
  build_populated(first);
  build_populated(second);

  // Swapping the head and the identity file of one store for another's is the
  // "unrelated store" case: the incarnation no longer matches.
  std::error_code error;
  std::filesystem::copy_file(second / "fcr.head", first / "fcr.head",
                             std::filesystem::copy_options::overwrite_existing, error);
  FCR_REQUIRE(!error);
  FCR_CHECK_ERROR(fcr::Store::open(first), fcr::ErrorCode::IncarnationMismatch);
}

FCR_TEST(recovery, a_corrupt_identity_file_is_refused) {
  fcr_test::TempDir directory("recovery_meta");
  const std::filesystem::path root = directory / "store";
  build_populated(root);
  fcr_test::write_text_file(root / "fcr.meta", "not a store identity at all\n");
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::MalformedRecord);

  fcr_test::write_text_file(root / "fcr.meta", "fcr-store 1\nincarnation=zzzz\n");
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::MalformedRecord);
}

FCR_TEST(recovery, an_identity_file_with_a_broken_seal_is_refused) {
  fcr_test::TempDir directory("recovery_seal");
  const std::filesystem::path root = directory / "store";
  build_populated(root);
  std::string meta = fcr_test::read_text_file(root / "fcr.meta");
  const std::size_t digest_position = meta.find("digest=sha256:");
  FCR_REQUIRE(digest_position != std::string::npos);
  // Flip one hexadecimal digit of the seal. The document keeps its shape and its
  // length, so only the digest check can refuse it.
  const std::size_t first_digit = digest_position + std::string("digest=sha256:").size();
  FCR_REQUIRE(first_digit < meta.size());
  meta[first_digit] = meta[first_digit] == '0' ? '1' : '0';
  fcr_test::write_text_file(root / "fcr.meta", meta);
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::DigestMismatch);
}

FCR_TEST(recovery, a_corrupt_head_pointer_is_distinguished_from_a_missing_one) {
  fcr_test::TempDir directory("recovery_head");
  const std::filesystem::path root = directory / "store";
  build_populated(root);
  const std::string good = fcr_test::read_text_file(root / "fcr.head");
  const std::string body = good.substr(0, good.rfind("digest=sha256:"));
  const auto reseal = [](const std::string& text) {
    return text + "digest=sha256:" + fcr::sha256_hex(text) + "\n";
  };

  // A head pointer that is not even terminated is truncated input.
  fcr_test::write_text_file(root / "fcr.head", "fcr-head 1");
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::TruncatedInput);

  // A head pointer with a repeated key is refused rather than resolved by first
  // match.
  fcr_test::write_text_file(root / "fcr.head", reseal(body + "revision=1\n"));
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::DuplicateField);

  // A head pointer missing a key is refused.
  std::string without_epoch;
  std::size_t position = 0;
  while (position < body.size()) {
    const std::size_t end = body.find('\n', position);
    const std::string line = body.substr(position, end - position);
    if (line.rfind("epoch=", 0) != 0) {
      without_epoch.append(line);
      without_epoch.push_back('\n');
    }
    position = end + 1;
  }
  fcr_test::write_text_file(root / "fcr.head", reseal(without_epoch));
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::MissingField);

  // Restoring the real head pointer makes the store openable again, so the
  // failures above were rejections and not damage.
  fcr_test::write_text_file(root / "fcr.head", good);
  fcr::Result<fcr::Store> opened = fcr::Store::open(root);
  FCR_REQUIRE(opened.has_value());
  opened->close();
}

FCR_TEST(recovery, a_head_pointer_with_a_zero_epoch_is_refused) {
  fcr_test::TempDir directory("recovery_epoch");
  const std::filesystem::path root = directory / "store";
  build_populated(root);
  std::string head = fcr_test::read_text_file(root / "fcr.head");
  const std::size_t position = head.find("epoch=");
  FCR_REQUIRE(position != std::string::npos);
  const std::size_t end = head.find('\n', position);
  const std::string body = head.substr(0, position) + "epoch=0" + head.substr(end);
  head = body + "digest=sha256:" + fcr::sha256_hex(body) + "\n";
  fcr_test::write_text_file(root / "fcr.head", head);
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::HeadCorrupt);
}

FCR_TEST(recovery, a_head_pointer_that_escapes_the_store_directory_is_refused) {
  fcr_test::TempDir directory("recovery_escape");
  const std::filesystem::path root = directory / "store";
  build_populated(root);
  std::string head = fcr_test::read_text_file(root / "fcr.head");
  const std::size_t position = head.find("state-file=");
  FCR_REQUIRE(position != std::string::npos);
  const std::size_t end = head.find('\n', position);
  const std::string body =
      head.substr(0, position) + "state-file=..\\..\\escape.fcr" + head.substr(end);
  head = body + "digest=sha256:" + fcr::sha256_hex(body) + "\n";
  fcr_test::write_text_file(root / "fcr.head", head);
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::PathEscapesRoot);
}

FCR_TEST(recovery, a_head_pointer_with_a_malformed_generation_name_is_refused) {
  fcr_test::TempDir directory("recovery_name");
  const std::filesystem::path root = directory / "store";
  build_populated(root);
  std::string head = fcr_test::read_text_file(root / "fcr.head");
  const std::size_t position = head.find("state-file=");
  const std::size_t end = head.find('\n', position);
  const std::string body = head.substr(0, position) + "state-file=state-x-y.fcr" + head.substr(end);
  head = body + "digest=sha256:" + fcr::sha256_hex(body) + "\n";
  fcr_test::write_text_file(root / "fcr.head", head);
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::PathEscapesRoot);
}

FCR_TEST(recovery, a_reopened_store_refuses_stale_authority_from_the_previous_run) {
  fcr_test::TempDir directory("recovery_epoch_stale");
  const std::filesystem::path root = directory / "store";
  fcr::AuthorityEpoch old_epoch(0);
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(snapshot_of(1), fcr::Tick(1'000)));
    old_epoch = created->epoch().value();
    created->close();
  }
  fcr::Result<fcr::Store> reopened = fcr::Store::open(root);
  FCR_REQUIRE(reopened.has_value());
  const fcr::Result<fcr::AuthorityEpoch> new_epoch = reopened->epoch();
  FCR_REQUIRE(new_epoch.has_value());
  FCR_CHECK_EQ(new_epoch->value(), old_epoch.value() + 1);

  fcr_test::Fixture fixture;
  fixture.snapshot = snapshot_of(1);
  fixture.epoch = old_epoch;
  FCR_CHECK_ERROR(reopened->reserve(fcr_test::reserve_request(
                      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100,
                      900'000)),
                  fcr::ErrorCode::StaleAuthorityEpoch);

  fixture.epoch = *new_epoch;
  FCR_CHECK_ERROR(reopened->reserve(fcr_test::reserve_request(
                      fixture, "res-1", "attempt-2", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100,
                      900'000)),
                  fcr::ErrorCode::CapacityEvidenceStale);
  reopened->close();
}

FCR_TEST(recovery, lifecycle_operations_work_against_restored_capacity) {
  fcr_test::TempDir directory("recovery_lifecycle");
  const std::filesystem::path root = directory / "store";
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(snapshot_of(1), fcr::Tick(1'000)));
    fcr_test::Fixture fixture;
    fixture.snapshot = snapshot_of(1);
    fixture.epoch = created->epoch().value();
    FCR_REQUIRE_OK(created->reserve(fcr_test::reserve_request(
        fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 2'000)));
    created->close();
  }

  fcr::Result<fcr::Store> reopened = fcr::Store::open(root);
  FCR_REQUIRE(reopened.has_value());
  fcr_test::Fixture fixture;
  fixture.snapshot = snapshot_of(1);
  fixture.epoch = reopened->epoch().value();

  // The deadline has passed while the store was closed; the sweep ends it.
  const fcr::Result<fcr::ExpireOutcome> expired =
      reopened->expire(fcr_test::expire_request(fixture, "sweep-1", 5'000));
  FCR_REQUIRE(expired.has_value());
  FCR_CHECK_EQ(expired->expired.size(), std::size_t{1});

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = reopened->pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ((*pools)[0].committed, 0ULL);
  FCR_CHECK_EQ((*pools)[0].free, 40ULL);
  reopened->close();

  // A second reopen sees the expiry.
  fcr::Result<fcr::Store> third = fcr::Store::open(root);
  FCR_REQUIRE(third.has_value());
  const fcr::Result<std::optional<fcr::ReservationView>> view = third->find(fcr_test::reservation_id("res-1"));
  FCR_REQUIRE(view.has_value());
  FCR_REQUIRE(view->has_value());
  FCR_CHECK((*view)->record.state == fcr::ReservationState::Expired);
  third->close();
}

FCR_TEST(recovery, an_attempt_recorded_before_a_restart_still_replays_after_it) {
  fcr_test::TempDir directory("recovery_attempt");
  const std::filesystem::path root = directory / "store";
  fcr::ReserveRequest request;
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(snapshot_of(1), fcr::Tick(1'000)));
    fcr_test::Fixture fixture;
    fixture.snapshot = snapshot_of(1);
    fixture.epoch = created->epoch().value();
    request = fcr_test::reserve_request(fixture, "res-1", "attempt-1",
                                        {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 4)}, 1'100, 900'000);
    FCR_REQUIRE_OK(created->reserve(request));
    created->close();
  }
  fcr::Result<fcr::Store> reopened = fcr::Store::open(root);
  FCR_REQUIRE(reopened.has_value());
  const fcr::Result<fcr::ReserveOutcome> replay = reopened->reserve(request);
  FCR_REQUIRE(replay.has_value());
  FCR_CHECK(replay->replayed);
  FCR_CHECK_EQ(replay->reservation.record.id.value(), std::string("res-1"));
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = reopened->pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ((*pools)[0].committed, 4ULL);

  // The replay was a projection, not a mutation: the epoch handover is the only
  // thing that changed, and it did so before the store accepted anything.
  const fcr::Result<fcr::Revision> revision = reopened->revision();
  FCR_REQUIRE(revision.has_value());
  FCR_CHECK_EQ(revision->value(), 2ULL);
  reopened->close();
}

FCR_TEST(recovery, an_empty_directory_is_not_a_store) {
  fcr_test::TempDir directory("recovery_empty");
  FCR_CHECK_ERROR(fcr::Store::open(directory.path()), fcr::ErrorCode::StoreNotFound);
  fcr::Result<fcr::Store> created = fcr::Store::create(directory.path());
  FCR_REQUIRE(created.has_value());
  created->close();
}
