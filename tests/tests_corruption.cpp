// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Corruption and adversarial input: the store refuses to adopt anything it
// cannot verify, and every rejection is a stable code rather than a crash.

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
};

fcr::CapacitySnapshot snapshot_of(std::uint64_t generation) {
  return fcr_test::make_snapshot("snap-corruption", "site-corruption", generation, generation * 1'000, kPools);
}

/// Builds a store with two reservations so corruption tests have real content.
void build_two(const std::filesystem::path& root) {
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  FCR_REQUIRE(created.has_value());
  FCR_REQUIRE_OK(created->install_capacity(snapshot_of(1), fcr::Tick(1'000)));
  fcr_test::Fixture fixture;
  fixture.snapshot = snapshot_of(1);
  fixture.epoch = created->epoch().value();
  FCR_REQUIRE_OK(created->reserve(fcr_test::reserve_request(
      fixture, "res-a", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 3)}, 1'100, 900'000)));
  FCR_REQUIRE_OK(created->reserve(fcr_test::reserve_request(
      fixture, "res-b", "attempt-2", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 4)}, 1'100, 900'000)));
  created->close();
}

}  // namespace

FCR_TEST(corruption, a_state_file_replaced_by_a_directory_is_refused) {
  fcr_test::TempDir directory("corrupt_directory");
  const std::filesystem::path root = directory / "store";
  build_two(root);
  const std::filesystem::path committed = fcr_test::committed_state_file(root);
  const std::string text = fcr_test::read_text_file(committed);
  std::error_code error;
  std::filesystem::remove(committed, error);
  std::filesystem::create_directory(committed, error);
  FCR_REQUIRE(!error);
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::StoreCorrupt);
  (void)text;
}

FCR_TEST(corruption, a_state_file_replaced_by_a_symlink_is_refused) {
  fcr_test::TempDir directory("corrupt_symlink");
  const std::filesystem::path root = directory / "store";
  build_two(root);
  const std::filesystem::path committed = fcr_test::committed_state_file(root);
  const std::string text = fcr_test::read_text_file(committed);
  const std::filesystem::path target = directory / "elsewhere.txt";
  fcr_test::write_text_file(target, text);
  std::error_code error;
  std::filesystem::remove(committed, error);
#if defined(_WIN32)
  // Creating a symbolic link needs a privilege the test process may not hold;
  // when it cannot be created the refusal cannot be exercised on this machine,
  // and that is reported rather than silently skipped.
  std::filesystem::create_symlink(target, committed, error);
  if (error) {
    fcr_test::note("symbolic link creation is unavailable in this environment; the refusal path was not exercised");
    return;
  }
#else
  std::filesystem::create_symlink(target, committed, error);
  FCR_REQUIRE(!error);
#endif
  const fcr::Result<fcr::Store> opened = fcr::Store::open(root);
  FCR_CHECK(!opened.has_value());
}

FCR_TEST(corruption, a_state_file_of_only_garbage_is_refused) {
  fcr_test::TempDir directory("corrupt_garbage");
  const std::filesystem::path root = directory / "store";
  build_two(root);
  const std::filesystem::path committed = fcr_test::committed_state_file(root);
  fcr_test::write_text_file(committed, std::string(4'096, '\x01'));
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::IntegrityFailure);
}

FCR_TEST(corruption, a_state_file_with_a_truncated_digest_line_is_refused) {
  fcr_test::TempDir directory("corrupt_digest");
  const std::filesystem::path root = directory / "store";
  build_two(root);
  const std::filesystem::path committed = fcr_test::committed_state_file(root);
  std::string text = fcr_test::read_text_file(committed);
  text.resize(text.size() - 10);
  fcr_test::write_text_file(committed, text);
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::IntegrityFailure);
}

FCR_TEST(corruption, a_rejected_open_leaves_the_store_intact_for_a_later_one) {
  fcr_test::TempDir directory("corrupt_recover");
  const std::filesystem::path root = directory / "store";
  build_two(root);
  const std::filesystem::path committed = fcr_test::committed_state_file(root);
  const std::string good = fcr_test::read_text_file(committed);

  fcr_test::write_text_file(committed, good.substr(0, good.size() / 2));
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::IntegrityFailure);

  fcr_test::write_text_file(committed, good);
  fcr::Result<fcr::Store> opened = fcr::Store::open(root);
  FCR_REQUIRE(opened.has_value());
  const fcr::Result<std::vector<fcr::ReservationView>> views = opened->list();
  FCR_REQUIRE(views.has_value());
  FCR_CHECK_EQ(views->size(), std::size_t{2});
  const fcr::Result<fcr::VerificationReport> report = opened->verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
  opened->close();
}

FCR_TEST(corruption, a_lock_file_replaced_by_a_symlink_is_refused) {
  fcr_test::TempDir directory("corrupt_lock_link");
  const std::filesystem::path root = directory / "store";
  build_two(root);
  const std::filesystem::path lock = root / "fcr.lock";
  std::error_code error;
  std::filesystem::remove(lock, error);
  FCR_REQUIRE(!error);

  const std::filesystem::path target = directory / "elsewhere.lock";
  fcr_test::write_text_file(target, "");
#if defined(_WIN32)
  // Creating a symbolic link needs a privilege the test process may not hold.
  // When it cannot be created, that is reported rather than silently skipped.
  std::filesystem::create_symlink(target, lock, error);
  if (error) {
    fcr_test::note("symbolic link creation is unavailable in this environment; the refusal path was not exercised");
    return;
  }
#else
  std::filesystem::create_symlink(target, lock, error);
  FCR_REQUIRE(!error);
#endif

  // Two processes locking different files while each believed it held the store
  // is exactly the failure the lock exists to prevent, so the link is refused.
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::SymlinkRefused);
  FCR_CHECK_ERROR(fcr::Store::open_read_only(root), fcr::ErrorCode::SymlinkRefused);
}

FCR_TEST(corruption, a_store_root_reached_through_a_symlink_is_refused) {
  fcr_test::TempDir directory("corrupt_root_link");
  const std::filesystem::path real_root = directory / "real-store";
  build_two(real_root);
  const std::filesystem::path link = directory / "linked-store";
  std::error_code error;
#if defined(_WIN32)
  std::filesystem::create_directory_symlink(real_root, link, error);
  if (error) {
    fcr_test::note("directory symbolic link creation is unavailable; the refusal path was not exercised");
    return;
  }
#else
  std::filesystem::create_directory_symlink(real_root, link, error);
  FCR_REQUIRE(!error);
#endif

  FCR_CHECK_ERROR(fcr::Store::open(link), fcr::ErrorCode::SymlinkRefused);
  FCR_CHECK_ERROR(fcr::Store::open_read_only(link), fcr::ErrorCode::SymlinkRefused);
  FCR_CHECK_ERROR(fcr::Store::create(link), fcr::ErrorCode::SymlinkRefused);

  // The real store is untouched and still opens.
  fcr::Result<fcr::Store> opened = fcr::Store::open(real_root);
  FCR_REQUIRE(opened.has_value());
  const fcr::Result<std::vector<fcr::ReservationView>> views = opened->list();
  FCR_REQUIRE(views.has_value());
  FCR_CHECK_EQ(views->size(), std::size_t{2});
  opened->close();
}

FCR_TEST(corruption, a_head_pointer_with_a_wrong_length_digest_is_refused) {
  fcr_test::TempDir directory("corrupt_head_digest");
  const std::filesystem::path root = directory / "store";
  build_two(root);
  std::string head = fcr_test::read_text_file(root / "fcr.head");
  fcr_test::patch_text(head, "state-digest=", "state-digest=00");
  fcr_test::write_text_file(root / "fcr.head", head);
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::DigestMismatch);
}

FCR_TEST(corruption, a_head_pointer_declaring_the_wrong_size_is_refused) {
  fcr_test::TempDir directory("corrupt_head_size");
  const std::filesystem::path root = directory / "store";
  build_two(root);
  std::string head = fcr_test::read_text_file(root / "fcr.head");
  const std::size_t position = head.find("state-bytes=");
  FCR_REQUIRE(position != std::string::npos);
  const std::size_t end = head.find('\n', position);
  const std::string body = head.substr(0, position) + "state-bytes=7" + head.substr(end);
  fcr_test::write_text_file(root / "fcr.head", body + "digest=sha256:" + fcr::sha256_hex(body) + "\n");
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::IntegrityFailure);
}

FCR_TEST(corruption, a_head_pointer_declaring_the_wrong_revision_is_refused) {
  fcr_test::TempDir directory("corrupt_head_revision");
  const std::filesystem::path root = directory / "store";
  build_two(root);
  std::string head = fcr_test::read_text_file(root / "fcr.head");
  const std::size_t position = head.find("revision=");
  FCR_REQUIRE(position != std::string::npos);
  const std::size_t end = head.find('\n', position);
  const std::string body = head.substr(0, position) + "revision=9999" + head.substr(end);
  fcr_test::write_text_file(root / "fcr.head", body + "digest=sha256:" + fcr::sha256_hex(body) + "\n");
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::StoreCorrupt);
}

FCR_TEST(corruption, an_oversized_state_file_is_refused_before_it_is_read) {
  fcr_test::TempDir directory("corrupt_oversized");
  const std::filesystem::path root = directory / "store";
  build_two(root);
  const std::uintmax_t size = std::filesystem::file_size(fcr_test::committed_state_file(root));
  FCR_REQUIRE(size > 32);

  // The declared bound is applied to the file's own reported size before any
  // content is allocated.
  fcr::StoreOptions tiny;
  tiny.limits.max_state_bytes = 32;
  FCR_CHECK_ERROR(fcr::Store::open(root, tiny), fcr::ErrorCode::LimitExceeded);

  fcr::StoreOptions generous;
  generous.limits.max_state_bytes = 64 * 1024;
  fcr::Result<fcr::Store> opened = fcr::Store::open(root, generous);
  FCR_REQUIRE(opened.has_value());
  opened->close();
}

FCR_TEST(corruption, a_directory_that_is_not_a_store_is_refused_by_every_entry_point) {
  fcr_test::TempDir directory("corrupt_foreign");
  const std::filesystem::path root = directory / "store";
  std::error_code error;
  std::filesystem::create_directories(root, error);
  FCR_REQUIRE(!error);

  // A store identity file that is not one: every entry point refuses it, and the
  // rejection is a stable code rather than an exception.
  fcr_test::write_text_file(root / "fcr.meta", "definitely not a store identity\n");
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::MalformedRecord);
  FCR_CHECK_ERROR(fcr::Store::open_read_only(root), fcr::ErrorCode::MalformedRecord);
  FCR_CHECK_ERROR(fcr::Store::create(root), fcr::ErrorCode::StoreNotEmpty);
}

FCR_TEST(corruption, an_unrelated_file_in_the_store_directory_is_ignored) {
  fcr_test::TempDir directory("corrupt_extra");
  const std::filesystem::path root = directory / "store";
  build_two(root);
  fcr_test::write_text_file(root / "notes.txt", "operator notes");
  fcr_test::write_text_file(root / "generations" / "state-1-deadbeefdeadbeef.fcr", "garbage");
  fcr::Result<fcr::Store> opened = fcr::Store::open(root);
  FCR_REQUIRE(opened.has_value());
  FCR_CHECK(fcr_test::file_exists(root / "notes.txt"));
  const fcr::Result<std::vector<fcr::ReservationView>> views = opened->list();
  FCR_REQUIRE(views.has_value());
  FCR_CHECK_EQ(views->size(), std::size_t{2});
  FCR_CHECK_EQ(opened->recovery().orphan_generations_removed, std::size_t{1});
  opened->close();
}

FCR_TEST(corruption, a_scope_that_looks_like_a_path_is_refused_by_the_claim_path) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ScopeRef> bad = fcr::ScopeRef::parse("..\\..\\windows", "pool scope");
  FCR_CHECK_ERROR(bad, fcr::ErrorCode::MalformedIdentifier);
  const fcr::Result<fcr::ScopeRef> absolute = fcr::ScopeRef::parse("C:temp", "pool scope");
  FCR_CHECK_ERROR(absolute, fcr::ErrorCode::MalformedIdentifier);
  const fcr::Result<fcr::ScopeRef> traversal = fcr::ScopeRef::parse("..", "pool scope");
  FCR_CHECK_ERROR(traversal, fcr::ErrorCode::MalformedIdentifier);
}

FCR_TEST(corruption, every_rejection_path_terminates_without_leaving_a_lock) {
  fcr_test::TempDir directory("corrupt_locks");
  const std::filesystem::path root = directory / "store";
  build_two(root);
  for (int attempt = 0; attempt < 8; ++attempt) {
    const fcr::Result<fcr::Store> refused = fcr::Store::open(directory / "absent");
    FCR_CHECK(!refused.has_value());
  }
  fcr::StoreOptions tiny;
  tiny.limits.max_meta_bytes = 4;
  for (int attempt = 0; attempt < 8; ++attempt) {
    FCR_CHECK_ERROR(fcr::Store::open(root, tiny), fcr::ErrorCode::LimitExceeded);
  }
  fcr::Result<fcr::Store> opened = fcr::Store::open(root);
  FCR_REQUIRE(opened.has_value());
  opened->close();
}
