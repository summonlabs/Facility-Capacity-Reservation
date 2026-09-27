// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The durable store: directory shape, atomic publication, close/reopen, writer
// authority handover, and the cleanup of retired generations.

#include <algorithm>
#include <iterator>
#include <set>
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
  return fcr_test::make_snapshot("snap-durable", "site-durable", generation, generation * 1'000, kPools);
}

std::vector<std::string> generation_files(const std::filesystem::path& root) {
  return fcr_test::list_names(root / "generations");
}

}  // namespace

FCR_TEST(store, create_lays_out_the_documented_directory) {
  fcr_test::TempDir directory("store_layout");
  const std::filesystem::path root = directory / "store";
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  FCR_REQUIRE(created.has_value());
  FCR_CHECK(created->durable());
  FCR_CHECK(!created->read_only());
  FCR_CHECK_EQ(created->root().string(), root.string());
  FCR_CHECK(fcr_test::file_exists(root / "fcr.meta"));
  FCR_CHECK(fcr_test::file_exists(root / "fcr.head"));
  FCR_CHECK(fcr_test::file_exists(root / "fcr.lock"));
  FCR_CHECK(!generation_files(root).empty());

  const fcr::RecoveryReport recovery = created->recovery();
  FCR_CHECK(!recovery.opened_existing);
  FCR_CHECK_EQ(recovery.revision.value(), 0ULL);
  created->close();
}

FCR_TEST(store, creating_over_an_existing_store_is_refused) {
  fcr_test::TempDir directory("store_exists");
  const std::filesystem::path root = directory / "store";
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    created->close();
  }
  FCR_CHECK_ERROR(fcr::Store::create(root), fcr::ErrorCode::StoreNotEmpty);
}

FCR_TEST(store, opening_a_directory_that_is_not_a_store_is_refused) {
  fcr_test::TempDir directory("store_missing");
  FCR_CHECK_ERROR(fcr::Store::open(directory / "absent"), fcr::ErrorCode::StoreNotFound);
  fcr::Result<fcr::Store> opened = fcr::Store::open(directory.path());
  FCR_CHECK_ERROR(opened, fcr::ErrorCode::StoreNotFound);
}

FCR_TEST(store, every_accepted_mutation_is_on_disk_before_the_call_returns) {
  fcr_test::TempDir directory("store_durable");
  const std::filesystem::path root = directory / "store";
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  FCR_REQUIRE(created.has_value());
  FCR_REQUIRE_OK(created->install_capacity(snapshot_of(1), fcr::Tick(1'000)));

  fcr_test::Fixture fixture;
  fixture.snapshot = snapshot_of(1);
  fixture.epoch = created->epoch().value();
  FCR_REQUIRE_OK(created->reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 900'000)));

  const fcr::Result<fcr::Revision> revision = created->revision();
  FCR_REQUIRE(revision.has_value());

  // Read the committed artifact while the writer is still open: the head must
  // already name a generation that holds the mutation.
  const std::filesystem::path committed = fcr_test::committed_state_file(root);
  const std::string document = fcr_test::read_text_file(committed);
  FCR_CHECK(document.find("reservation.id=res-1") != std::string::npos);
  FCR_CHECK(document.find("revision=" + fcr::format_unsigned(revision->value())) != std::string::npos);
  FCR_CHECK_EQ(fcr::sha256_hex(document),
               fcr::sha256_hex(fcr_test::read_text_file(fcr_test::committed_state_file(root))));
  created->close();
}

FCR_TEST(store, close_and_reopen_preserves_the_state_exactly) {
  fcr_test::TempDir directory("store_reopen");
  const std::filesystem::path root = directory / "store";
  std::string fingerprint_before;
  fcr::Revision revision_before(0);
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(snapshot_of(2), fcr::Tick(1'000)));
    fcr_test::Fixture fixture;
    fixture.snapshot = snapshot_of(2);
    fixture.epoch = created->epoch().value();
    for (int index = 0; index < 5; ++index) {
      const std::string id = "res-" + std::to_string(index);
      const std::string attempt_name = "attempt-" + std::to_string(index);
      FCR_REQUIRE_OK(created->reserve(fcr_test::reserve_request(
          fixture, id.c_str(), attempt_name.c_str(), {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 2)}, 1'100,
          900'000)));
    }
    const fcr::Result<fcr::Revision> revision = created->revision();
    FCR_REQUIRE(revision.has_value());
    revision_before = *revision;
    const fcr::Result<std::vector<fcr::ReservationView>> views = created->list();
    FCR_REQUIRE(views.has_value());
    fingerprint_before = fcr::sha256_hex([&views] {
      std::string material;
      for (const fcr::ReservationView& view : *views) {
        material.append(view.record.id.value());
        material.push_back('|');
        material.append(fcr::format_unsigned(view.record.generation.value()));
        material.push_back(';');
      }
      return material;
    }());
    created->close();
  }

  fcr::Result<fcr::Store> reopened = fcr::Store::open(root);
  FCR_REQUIRE(reopened.has_value());
  const fcr::Result<fcr::Revision> revision_after = reopened->revision();
  FCR_REQUIRE(revision_after.has_value());
  FCR_CHECK_EQ(revision_after->value(), revision_before.value());
  const fcr::Result<std::vector<fcr::ReservationView>> views = reopened->list();
  FCR_REQUIRE(views.has_value());
  FCR_CHECK_EQ(views->size(), std::size_t{5});
  FCR_CHECK_EQ(fcr::sha256_hex([&views] {
                 std::string material;
                 for (const fcr::ReservationView& view : *views) {
                   material.append(view.record.id.value());
                   material.push_back('|');
                   material.append(fcr::format_unsigned(view.record.generation.value()));
                   material.push_back(';');
                 }
                 return material;
               }()),
               fingerprint_before);
  const fcr::RecoveryReport recovery = reopened->recovery();
  FCR_CHECK(recovery.opened_existing);
  FCR_CHECK(recovery.recovered);
  FCR_CHECK_EQ(recovery.current_epoch.value(), recovery.previous_epoch.value() + 1);
  reopened->close();
}

FCR_TEST(store, each_reopen_advances_the_writer_authority_epoch) {
  fcr_test::TempDir directory("store_epoch");
  const std::filesystem::path root = directory / "store";
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_CHECK_EQ(created->epoch().value().value(), 1ULL);
    created->close();
  }
  for (std::uint64_t expected = 2; expected <= 5; ++expected) {
    fcr::Result<fcr::Store> opened = fcr::Store::open(root);
    FCR_REQUIRE(opened.has_value());
    FCR_CHECK_EQ(opened->epoch().value().value(), expected);
    opened->close();
  }
}

FCR_TEST(store, only_the_head_generation_is_retained) {
  fcr_test::TempDir directory("store_generations");
  const std::filesystem::path root = directory / "store";
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  FCR_REQUIRE(created.has_value());
  FCR_REQUIRE_OK(created->install_capacity(snapshot_of(1), fcr::Tick(1'000)));
  fcr_test::Fixture fixture;
  fixture.snapshot = snapshot_of(1);
  fixture.epoch = created->epoch().value();
  for (int index = 0; index < 10; ++index) {
    const std::string id = "res-" + std::to_string(index);
    const std::string attempt_name = "attempt-" + std::to_string(index);
    FCR_REQUIRE_OK(created->reserve(fcr_test::reserve_request(
        fixture, id.c_str(), attempt_name.c_str(), {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100,
        900'000)));
  }
  const std::vector<std::string> files = generation_files(root);
  FCR_CHECK_EQ(files.size(), std::size_t{1});
  const std::filesystem::path committed = fcr_test::committed_state_file(root);
  FCR_CHECK_EQ(committed.filename().string(), files[0]);
  created->close();
}

FCR_TEST(store, an_obsolete_generation_left_behind_is_removed_on_open) {
  fcr_test::TempDir directory("store_orphan");
  const std::filesystem::path root = directory / "store";
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(snapshot_of(1), fcr::Tick(1'000)));
    created->close();
  }
  const std::filesystem::path orphan = root / "generations" / "state-999-0123456789abcdef.fcr";
  fcr_test::write_text_file(orphan, "fcr-state 1\n");
  FCR_CHECK(fcr_test::file_exists(orphan));

  fcr::Result<fcr::Store> opened = fcr::Store::open(root);
  FCR_REQUIRE(opened.has_value());
  FCR_CHECK(!fcr_test::file_exists(orphan));
  FCR_CHECK_EQ(opened->recovery().orphan_generations_removed, std::size_t{1});
  opened->close();
}

FCR_TEST(store, a_file_the_store_did_not_write_is_never_removed) {
  fcr_test::TempDir directory("store_foreign");
  const std::filesystem::path root = directory / "store";
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  FCR_REQUIRE(created.has_value());
  const std::filesystem::path foreign = root / "generations" / "operator-notes.txt";
  fcr_test::write_text_file(foreign, "do not delete me");
  FCR_REQUIRE_OK(created->install_capacity(snapshot_of(1), fcr::Tick(1'000)));
  FCR_CHECK(fcr_test::file_exists(foreign));
  FCR_CHECK_EQ(fcr_test::read_text_file(foreign), std::string("do not delete me"));
  created->close();
}

FCR_TEST(store, a_second_writer_is_refused_while_the_first_holds_the_store) {
  fcr_test::TempDir directory("store_locked");
  const std::filesystem::path root = directory / "store";
  fcr::Result<fcr::Store> first = fcr::Store::create(root);
  FCR_REQUIRE(first.has_value());

  // A second in-process handle is refused too: the lock is the store's, not the
  // thread's.
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::StoreLocked);
  FCR_CHECK_ERROR(fcr::Store::open_read_only(root), fcr::ErrorCode::StoreLocked);

  first->close();
  fcr::Result<fcr::Store> second = fcr::Store::open(root);
  FCR_REQUIRE(second.has_value());
  second->close();
}

FCR_TEST(store, several_observers_may_share_a_store) {
  fcr_test::TempDir directory("store_readers");
  const std::filesystem::path root = directory / "store";
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(snapshot_of(1), fcr::Tick(1'000)));
    created->close();
  }
  fcr::Result<fcr::Store> first = fcr::Store::open_read_only(root);
  FCR_REQUIRE(first.has_value());
  fcr::Result<fcr::Store> second = fcr::Store::open_read_only(root);
  FCR_REQUIRE(second.has_value());
  FCR_CHECK_EQ(first->revision().value().value(), second->revision().value().value());
  // An observer excludes a writer and vice versa.
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::StoreLocked);
  first->close();
  second->close();
}

FCR_TEST(store, a_volatile_store_is_independent_of_any_directory) {
  fcr::Result<fcr::Store> first = fcr::Store::in_memory();
  FCR_REQUIRE(first.has_value());
  fcr::Result<fcr::Store> second = fcr::Store::in_memory();
  FCR_REQUIRE(second.has_value());
  FCR_CHECK_NE(first->incarnation().value().to_string(), second->incarnation().value().to_string());
  FCR_REQUIRE_OK(first->install_capacity(snapshot_of(1), fcr::Tick(1'000)));
  FCR_CHECK_ERROR(second->capacity(), fcr::ErrorCode::NoCapacityInstalled);
}

FCR_TEST(store, a_volatile_store_never_writes_anything_anywhere) {
  // A volatile store has no directory, so a mutation must not create a file —
  // not in a store directory, and above all not in the process's working
  // directory under a name derived from the revision.
  const std::filesystem::path working = std::filesystem::current_path();
  const std::vector<std::string> before = fcr_test::list_names(working);

  {
    fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
    FCR_CHECK(!fixture.store.durable());
    FCR_CHECK(fixture.store.root().empty());
    for (int index = 0; index < 20; ++index) {
      const std::string id = "res-volatile-" + fcr::format_unsigned(static_cast<std::uint64_t>(index));
      const std::string attempt_name = "attempt-volatile-" + fcr::format_unsigned(static_cast<std::uint64_t>(index));
      FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
          fixture, id.c_str(), attempt_name.c_str(), {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)},
          1'100, 900'000)));
    }
    FCR_REQUIRE_OK(fixture.store.expire(fcr_test::expire_request(fixture, "sweep-volatile", 5'000)));
    FCR_REQUIRE_OK(fixture.store.reconcile(fcr_test::reconcile_request(
        fixture, "reconcile-volatile", snapshot_of(2), fcr::SourceGeneration(1), fcr::ReconcileMode::Enforce)));
  }

  const std::vector<std::string> after = fcr_test::list_names(working);
  std::vector<std::string> created;
  std::set_difference(after.begin(), after.end(), before.begin(), before.end(), std::back_inserter(created));
  for (const std::string& name : created) {
    FCR_FAIL("a volatile store created a file in the working directory: " + name);
  }
}

FCR_TEST(store, a_moved_store_keeps_working_and_the_source_is_closed) {
  fcr_test::TempDir directory("store_move");
  const std::filesystem::path root = directory / "store";
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  FCR_REQUIRE(created.has_value());
  fcr::Store moved = std::move(created).value();
  FCR_CHECK(created->closed());
  FCR_REQUIRE_OK(moved.install_capacity(snapshot_of(1), fcr::Tick(1'000)));
  FCR_CHECK_EQ(moved.revision().value().value(), 1ULL);
  moved.close();
}

FCR_TEST(store, verify_re_reads_the_committed_artifact) {
  fcr_test::TempDir directory("store_verify");
  const std::filesystem::path root = directory / "store";
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  FCR_REQUIRE(created.has_value());
  FCR_REQUIRE_OK(created->install_capacity(snapshot_of(1), fcr::Tick(1'000)));
  const fcr::Result<fcr::VerificationReport> report = created->verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);

  // Corrupting the committed generation underneath the open store must be
  // detected by verify() even though the in-memory state is unaffected.
  const std::filesystem::path committed = fcr_test::committed_state_file(root);
  fcr_test::append_file(committed, "trailing=1\n");
  FCR_CHECK_ERROR(created->verify(), fcr::ErrorCode::IntegrityFailure);
  created->close();
}

FCR_TEST(store, a_reserve_outcome_is_identical_across_a_reopen) {
  fcr_test::TempDir directory("store_outcome");
  const std::filesystem::path root = directory / "store";
  fcr::Revision revision(0);
  fcr::ReservationGeneration generation(0);
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(snapshot_of(1), fcr::Tick(1'000)));
    fcr_test::Fixture fixture;
    fixture.snapshot = snapshot_of(1);
    fixture.epoch = created->epoch().value();
    const fcr::Result<fcr::ReserveOutcome> outcome = created->reserve(fcr_test::reserve_request(
        fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 3)}, 1'100, 900'000));
    FCR_REQUIRE(outcome.has_value());
    revision = outcome->revision;
    generation = outcome->reservation.record.generation;
    created->close();
  }
  fcr::Result<fcr::Store> reopened = fcr::Store::open(root);
  FCR_REQUIRE(reopened.has_value());
  const fcr::Result<std::optional<fcr::ReservationView>> view =
      reopened->find(fcr_test::reservation_id("res-1"));
  FCR_REQUIRE(view.has_value());
  FCR_REQUIRE(view->has_value());
  FCR_CHECK_EQ((*view)->record.generation.value(), generation.value());
  FCR_CHECK_EQ((*view)->record.revision.value(), revision.value());
  FCR_CHECK_EQ((*view)->record.claims.size(), std::size_t{1});
  // The commitment was priced against the same capacity generation, so it is not
  // stale; what changed is that the evidence itself is no longer known to be
  // current, which the store reports separately.
  FCR_CHECK(!(*view)->source_stale);
  const fcr::Result<fcr::LedgerStatus> status = reopened->status();
  FCR_REQUIRE(status.has_value());
  FCR_CHECK(!status->capacity_fresh);
  reopened->close();
}

FCR_TEST(store, the_persisted_tick_is_restored) {
  fcr_test::TempDir directory("store_tick");
  const std::filesystem::path root = directory / "store";
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(snapshot_of(1), fcr::Tick(1'234)));
    created->close();
  }
  fcr::Result<fcr::Store> reopened = fcr::Store::open(root);
  FCR_REQUIRE(reopened.has_value());
  const fcr::Result<fcr::Tick> tick = reopened->last_observed_tick();
  FCR_REQUIRE(tick.has_value());
  FCR_CHECK_EQ(tick->value(), 1'234ULL);
  reopened->close();
}

FCR_TEST(store, status_reports_durability_and_closure) {
  fcr_test::TempDir directory("store_status");
  const std::filesystem::path root = directory / "store";
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  FCR_REQUIRE(created.has_value());
  const fcr::Result<fcr::LedgerStatus> status = created->status();
  FCR_REQUIRE(status.has_value());
  FCR_CHECK(status->durable);
  FCR_CHECK(!status->closed);
  created->close();
  FCR_CHECK_ERROR(created->status(), fcr::ErrorCode::StoreClosed);
}

FCR_TEST(store, a_store_created_with_a_custom_bound_enforces_it_after_reopen) {
  fcr_test::TempDir directory("store_bounds");
  const std::filesystem::path root = directory / "store";
  {
    fcr::StoreOptions options;
    options.ledger.max_attempts = 2;
    fcr::Result<fcr::Store> created = fcr::Store::create(root, options);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(snapshot_of(1), fcr::Tick(1'000)));
    fcr_test::Fixture fixture;
    fixture.snapshot = snapshot_of(1);
    fixture.epoch = created->epoch().value();
    for (int index = 0; index < 5; ++index) {
      const std::string id = "res-" + std::to_string(index);
      const std::string attempt_name = "attempt-" + std::to_string(index);
      FCR_REQUIRE_OK(created->reserve(fcr_test::reserve_request(
          fixture, id.c_str(), attempt_name.c_str(), {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100,
          900'000)));
    }
    created->close();
  }
  // Reopening with a bound smaller than the persisted attempt index is refused
  // rather than silently truncated.
  fcr::StoreOptions tiny;
  tiny.ledger.max_attempts = 1;
  FCR_CHECK_ERROR(fcr::Store::open(root, tiny), fcr::ErrorCode::LimitExceeded);

  fcr::Result<fcr::Store> reopened = fcr::Store::open(root);
  FCR_REQUIRE(reopened.has_value());
  FCR_CHECK_EQ(reopened->status().value().attempt_count, std::size_t{2});
  reopened->close();
}
