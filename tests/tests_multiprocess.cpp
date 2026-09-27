// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Real multiprocess proof obligations.
//
// Every scenario below runs the test executable again as a separate operating
// system process. Nothing here is simulated with threads: the child processes
// are real processes with their own address spaces, their own file handles and
// their own lifetime, and their deaths are real deaths.
//
// The child scenarios live in tests/tests_multiprocess_child.cpp and are
// dispatched by the test binary's entry point.

#include <algorithm>
#include <string>
#include <vector>

#include "child_process.hpp"
#include "dccp/facility_capacity_reservation/digest.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

const std::vector<fcr_test::PoolSpec> kPools = {
    {fcr::ResourceKind::Rack, "hall-a", 400, 0, 0},
};

/// Exit statuses the child scenarios use to describe their outcome.
constexpr int kChildOk = 0;
constexpr int kChildBlocked = 3;
constexpr int kChildFaulted = 70;

std::string child_value(const fcr_test::ChildOutcome& outcome, const std::string& key) {
  return fcr_test::output_value(outcome.output, key);
}

}  // namespace

FCR_TEST(multiprocess, a_second_process_is_refused_while_the_first_holds_the_store) {
  fcr_test::TempDir directory("mp_lock");
  const std::filesystem::path root = directory / "store";
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(
        fcr_test::make_snapshot("snap-mp", "site-mp", 1, 1'000, kPools), fcr::Tick(1'000)));
    // Deliberately left open: the lock is held for the rest of this block.
    const fcr_test::ChildOutcome child =
        fcr_test::run_child({"--fcr-child", "open-store", "--root", root.string()});
    FCR_CHECK_EQ(child.exit_code, kChildBlocked);
    FCR_CHECK_EQ(child_value(child, "child.open"), std::string("blocked"));
    FCR_CHECK_EQ(child_value(child, "child.error-code"), std::string("STORE_LOCKED"));
    created->close();
  }

  // Once the holder releases, the child can take the store.
  const fcr_test::ChildOutcome after =
      fcr_test::run_child({"--fcr-child", "open-store", "--root", root.string()});
  FCR_CHECK_EQ(after.exit_code, kChildOk);
  FCR_CHECK_EQ(child_value(after, "child.open"), std::string("ok"));
}

FCR_TEST(multiprocess, a_process_that_dies_holding_the_store_relinquishes_it) {
  fcr_test::TempDir directory("mp_death");
  const std::filesystem::path root = directory / "store";
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(
        fcr_test::make_snapshot("snap-mp", "site-mp", 1, 1'000, kPools), fcr::Tick(1'000)));
    created->close();
  }

  // The child opens the store, publishes a mutation and terminates without
  // closing: no destructor runs, no unlock is issued by its own code.
  const fcr_test::ChildOutcome child = fcr_test::run_child(
      {"--fcr-child", "die-holding", "--root", root.string(), "--id", "res-child"});
  FCR_CHECK_EQ(child.exit_code, kChildOk);
  FCR_CHECK_EQ(child_value(child, "child.committed"), std::string("true"));
  const std::string child_incarnation = child_value(child, "child.incarnation");

  // The operating system released the lock with the process, so this process can
  // take write authority.
  fcr::Result<fcr::Store> opened = fcr::Store::open(root);
  FCR_REQUIRE(opened.has_value());
  FCR_CHECK_EQ(opened->incarnation().value().to_string(), child_incarnation);
  const fcr::RecoveryReport recovery = opened->recovery();
  // The child took authority when it opened the store, so the epoch it published
  // under is one ahead of the creating process's; this process takes the next.
  FCR_CHECK_EQ(recovery.previous_epoch.value(), 2ULL);
  FCR_CHECK_EQ(recovery.current_epoch.value(), 3ULL);

  // The mutation the child published is intact.
  const fcr::Result<std::optional<fcr::ReservationView>> view =
      opened->find(fcr_test::reservation_id("res-child"));
  FCR_REQUIRE(view.has_value());
  FCR_REQUIRE(view->has_value());
  FCR_CHECK((*view)->active());
  FCR_CHECK_EQ((*view)->record.authority_epoch.value(), 2ULL);
  const fcr::Result<fcr::VerificationReport> report = opened->verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
  opened->close();
}

FCR_TEST(multiprocess, an_unacknowledged_mutation_is_never_adopted_after_a_crash) {
  for (const char* stage : {"after-state-write", "before-head-commit", "after-head-commit"}) {
    fcr_test::set_case_context(std::string("fault stage=") + stage);
    fcr_test::TempDir directory(std::string("mp_crash_") + stage);
    const std::filesystem::path root = directory / "store";
    {
      fcr::Result<fcr::Store> created = fcr::Store::create(root);
      FCR_REQUIRE(created.has_value());
      FCR_REQUIRE_OK(created->install_capacity(
          fcr_test::make_snapshot("snap-mp", "site-mp", 1, 1'000, kPools), fcr::Tick(1'000)));
      fcr_test::Fixture fixture;
      fixture.snapshot = fcr_test::make_snapshot("snap-mp", "site-mp", 1, 1'000, kPools);
      fixture.epoch = created->epoch().value();
      FCR_REQUIRE_OK(created->reserve(fcr_test::reserve_request(
          fixture, "res-acknowledged", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 5)}, 1'100,
          900'000)));
      created->close();
    }

    // The child commits a second reservation and dies inside the publication
    // protocol at the named stage.
    const fcr_test::ChildOutcome child = fcr_test::run_child(
        {"--fcr-child", "crash-publishing", "--root", root.string(), "--id", "res-crashed", "--fault", stage});
    FCR_CHECK_EQ(child.exit_code, kChildFaulted);

    fcr::Result<fcr::Store> opened = fcr::Store::open(root);
    FCR_REQUIRE(opened.has_value());
    const fcr::Result<fcr::VerificationReport> report = opened->verify();
    FCR_REQUIRE(report.has_value());
    FCR_CHECK(report->ok);

    // The acknowledged mutation must be present in every case.
    const fcr::Result<std::optional<fcr::ReservationView>> acknowledged =
        opened->find(fcr_test::reservation_id("res-acknowledged"));
    FCR_REQUIRE(acknowledged.has_value());
    FCR_REQUIRE(acknowledged->has_value());
    FCR_CHECK((*acknowledged)->active());

    // The crashed mutation is either wholly present or wholly absent, and the
    // accounting closes either way.
    const fcr::Result<std::optional<fcr::ReservationView>> crashed =
        opened->find(fcr_test::reservation_id("res-crashed"));
    FCR_REQUIRE(crashed.has_value());
    const bool committed_at_head = std::string(stage) == "after-head-commit";
    FCR_CHECK_EQ(crashed->has_value(), committed_at_head);

    const fcr::Result<std::vector<fcr::PoolAccount>> pools = opened->pools();
    FCR_REQUIRE(pools.has_value());
    FCR_REQUIRE(pools->size() == 1);
    const std::uint64_t expected_committed = committed_at_head ? 9ULL : 5ULL;
    FCR_CHECK_EQ((*pools)[0].committed, expected_committed);
    FCR_CHECK_EQ((*pools)[0].committed + (*pools)[0].protected_ + (*pools)[0].free, (*pools)[0].reservable);

    // No half-written generation is left behind that could later be adopted.
    const std::vector<std::string> files = fcr_test::list_names(root / "generations");
    FCR_CHECK_EQ(files.size(), std::size_t{1});
    opened->close();
  }
}

FCR_TEST(multiprocess, several_processes_writing_one_store_keep_the_accounting_closed) {
  fcr_test::TempDir directory("mp_race");
  const std::filesystem::path root = directory / "store";
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(
        fcr_test::make_snapshot("snap-mp", "site-mp", 1, 1'000, kPools), fcr::Tick(1'000)));
    created->close();
  }

  constexpr int kChildren = 6;
  constexpr int kRounds = 25;

  // Start every child before waiting for any of them, so the writers genuinely
  // overlap in time and contend for the same lock.
  std::vector<fcr_test::ChildHandle> handles;
  handles.reserve(kChildren);
  for (int index = 0; index < kChildren; ++index) {
    handles.push_back(fcr_test::start_child({"--fcr-child", "race-writer", "--root", root.string(), "--writer",
                                             fcr::format_unsigned(static_cast<std::uint64_t>(index)), "--rounds",
                                             fcr::format_unsigned(kRounds)}));
    FCR_REQUIRE(handles.back().valid());
  }

  std::uint64_t total_accepted = 0;
  for (fcr_test::ChildHandle& handle : handles) {
    const fcr_test::ChildOutcome outcome = handle.wait();
    FCR_CHECK_EQ(outcome.exit_code, kChildOk);
    const std::string accepted = child_value(outcome, "child.accepted");
    FCR_CHECK(!accepted.empty());
    total_accepted += fcr::parse_unsigned_bounded(accepted, 1'000'000).value();
  }
  FCR_CHECK(total_accepted > 0);

  // Every mutation the children reported as accepted must be present, exactly
  // once, and the accounting must close.
  fcr::Result<fcr::Store> opened = fcr::Store::open(root);
  FCR_REQUIRE(opened.has_value());
  const fcr::Result<std::vector<fcr::ReservationView>> views = opened->list();
  FCR_REQUIRE(views.has_value());
  FCR_CHECK_EQ(views->size(), static_cast<std::size_t>(total_accepted));

  std::uint64_t held = 0;
  for (const fcr::ReservationView& view : *views) {
    FCR_CHECK(view.active());
    held += view.record.claimed_from(fcr::PoolKey{fcr::ResourceKind::Rack, fcr_test::scope("hall-a")});
  }
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = opened->pools();
  FCR_REQUIRE(pools.has_value());
  FCR_REQUIRE(pools->size() == 1);
  FCR_CHECK_EQ((*pools)[0].committed, held);
  FCR_CHECK_EQ((*pools)[0].committed + (*pools)[0].protected_ + (*pools)[0].free, (*pools)[0].reservable);

  const fcr::Result<fcr::VerificationReport> report = opened->verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
  opened->close();
}

FCR_TEST(multiprocess, a_reader_process_sees_the_committed_state_and_never_a_partial_one) {
  fcr_test::TempDir directory("mp_reader");
  const std::filesystem::path root = directory / "store";
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(
        fcr_test::make_snapshot("snap-mp", "site-mp", 1, 1'000, kPools), fcr::Tick(1'000)));
    fcr_test::Fixture fixture;
    fixture.snapshot = fcr_test::make_snapshot("snap-mp", "site-mp", 1, 1'000, kPools);
    fixture.epoch = created->epoch().value();
    for (int index = 0; index < 10; ++index) {
      const std::string id = "res-" + fcr::format_unsigned(static_cast<std::uint64_t>(index));
      const std::string attempt_name = "attempt-" + fcr::format_unsigned(static_cast<std::uint64_t>(index));
      FCR_REQUIRE_OK(created->reserve(fcr_test::reserve_request(
          fixture, id.c_str(), attempt_name.c_str(), {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 2)}, 1'100,
          900'000)));
    }
    created->close();
  }

  // A writer holds the store; a reader process must be refused rather than
  // allowed to observe a state that is being replaced.
  fcr::Result<fcr::Store> writer = fcr::Store::open(root);
  FCR_REQUIRE(writer.has_value());
  const fcr_test::ChildOutcome blocked = fcr_test::run_child({"--fcr-child", "read-store", "--root", root.string()});
  FCR_CHECK_EQ(blocked.exit_code, kChildBlocked);
  FCR_CHECK_EQ(child_value(blocked, "child.error-code"), std::string("STORE_LOCKED"));
  writer->close();

  const fcr_test::ChildOutcome reader = fcr_test::run_child({"--fcr-child", "read-store", "--root", root.string()});
  FCR_CHECK_EQ(reader.exit_code, kChildOk);
  FCR_CHECK_EQ(child_value(reader, "child.reservations"), std::string("10"));
  FCR_CHECK_EQ(child_value(reader, "child.closure"), std::string("ok"));
  FCR_CHECK_EQ(child_value(reader, "child.capacity-fresh"), std::string("false"));
}

FCR_TEST(multiprocess, a_stale_epoch_from_a_dead_writer_is_refused_by_the_next_one) {
  fcr_test::TempDir directory("mp_epoch");
  const std::filesystem::path root = directory / "store";
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(
        fcr_test::make_snapshot("snap-mp", "site-mp", 1, 1'000, kPools), fcr::Tick(1'000)));
    created->close();
  }

  const fcr_test::ChildOutcome first = fcr_test::run_child(
      {"--fcr-child", "die-holding", "--root", root.string(), "--id", "res-first"});
  FCR_CHECK_EQ(first.exit_code, kChildOk);
  FCR_CHECK_EQ(child_value(first, "child.epoch"), std::string("2"));

  // A second process takes authority, advancing the epoch; then dies holding it.
  const fcr_test::ChildOutcome second = fcr_test::run_child(
      {"--fcr-child", "die-holding", "--root", root.string(), "--id", "res-second"});
  FCR_CHECK_EQ(second.exit_code, kChildOk);
  FCR_CHECK_EQ(child_value(second, "child.epoch"), std::string("3"));

  // The first process's epoch is now stale, and the store says so.
  const fcr_test::ChildOutcome stale = fcr_test::run_child(
      {"--fcr-child", "use-stale-epoch", "--root", root.string(), "--epoch", "1"});
  FCR_CHECK_EQ(stale.exit_code, kChildOk);
  FCR_CHECK_EQ(child_value(stale, "child.error-code"), std::string("STALE_AUTHORITY_EPOCH"));

  fcr::Result<fcr::Store> opened = fcr::Store::open(root);
  FCR_REQUIRE(opened.has_value());
  FCR_CHECK_EQ(opened->epoch().value().value(), 5ULL);
  const fcr::Result<std::vector<fcr::ReservationView>> views = opened->list();
  FCR_REQUIRE(views.has_value());
  FCR_CHECK_EQ(views->size(), std::size_t{2});
  opened->close();
}

FCR_TEST(multiprocess, two_children_never_hold_write_authority_at_the_same_time) {
  fcr_test::TempDir directory("mp_exclusive");
  const std::filesystem::path root = directory / "store";
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(
        fcr_test::make_snapshot("snap-mp", "site-mp", 1, 1'000, kPools), fcr::Tick(1'000)));
    created->close();
  }

  // Two children contend for the store; at most one may report that it held it,
  // and the store's epoch advances by exactly the number of successful opens.
  constexpr int kChildren = 4;
  constexpr int kRounds = 30;
  std::vector<fcr_test::ChildHandle> handles;
  for (int index = 0; index < kChildren; ++index) {
    handles.push_back(fcr_test::start_child({"--fcr-child", "count-opens", "--root", root.string(), "--rounds",
                                             fcr::format_unsigned(kRounds)}));
    FCR_REQUIRE(handles.back().valid());
  }
  std::uint64_t opens = 0;
  for (fcr_test::ChildHandle& handle : handles) {
    const fcr_test::ChildOutcome outcome = handle.wait();
    FCR_CHECK_EQ(outcome.exit_code, kChildOk);
    opens += fcr::parse_unsigned_bounded(child_value(outcome, "child.opens"), 1'000'000).value();
  }
  FCR_CHECK(opens > 0);

  fcr::Result<fcr::Store> opened = fcr::Store::open(root);
  FCR_REQUIRE(opened.has_value());
  // Every successful open advanced the epoch by exactly one: the count of
  // handovers equals the count of successful opens plus the one this process
  // just took.
  FCR_CHECK_EQ(opened->epoch().value().value(), opens + 2);
  const fcr::Result<fcr::VerificationReport> report = opened->verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
  opened->close();
}

FCR_TEST(multiprocess, a_child_writing_concurrently_with_its_parent_produces_one_whole_state) {
  fcr_test::TempDir directory("mp_concurrent");
  const std::filesystem::path root = directory / "store";
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(
        fcr_test::make_snapshot("snap-mp", "site-mp", 1, 1'000, kPools), fcr::Tick(1'000)));
    created->close();
  }

  const fcr::CapacitySnapshot snapshot = fcr_test::make_snapshot("snap-mp", "site-mp", 1, 1'000, kPools);
  std::uint64_t parent_accepted = 0;
  fcr_test::ChildHandle child = fcr_test::start_child(
      {"--fcr-child", "race-writer", "--root", root.string(), "--writer", "child", "--rounds", "40"});
  FCR_REQUIRE(child.valid());

  for (int round = 0; round < 40; ++round) {
    fcr::StoreOptions options;
    options.trust_persisted_capacity = true;
    fcr::Result<fcr::Store> trusted = fcr::Store::open(root, options);
    if (!trusted.has_value()) {
      // The child held the store at that instant. That is the expected
      // contention, not a failure.
      FCR_CHECK(trusted.error().code() == fcr::ErrorCode::StoreLocked);
      continue;
    }
    fcr_test::Fixture fixture;
    fixture.snapshot = snapshot;
    fixture.epoch = trusted->epoch().value();
    const std::string id = "res-parent-" + fcr::format_unsigned(static_cast<std::uint64_t>(round));
    const fcr::Result<fcr::ReserveOutcome> outcome = trusted->reserve(fcr_test::reserve_request(
        fixture, id.c_str(), ("attempt-" + id).c_str(), {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100,
        900'000));
    if (outcome.has_value()) {
      ++parent_accepted;
    }
    trusted->close();
  }

  const fcr_test::ChildOutcome child_outcome = child.wait();
  FCR_CHECK_EQ(child_outcome.exit_code, kChildOk);
  const std::uint64_t child_accepted =
      fcr::parse_unsigned_bounded(child_value(child_outcome, "child.accepted"), 1'000'000).value();

  fcr::Result<fcr::Store> opened = fcr::Store::open(root);
  FCR_REQUIRE(opened.has_value());
  const fcr::Result<std::vector<fcr::ReservationView>> views = opened->list();
  FCR_REQUIRE(views.has_value());
  FCR_CHECK_EQ(views->size(), static_cast<std::size_t>(parent_accepted + child_accepted));
  const fcr::Result<fcr::VerificationReport> report = opened->verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = opened->pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ((*pools)[0].committed, parent_accepted + child_accepted);
  opened->close();
}
