// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Accounting closure against an independent reference model, including the
// settled operations that return capacity, the attempt index bound, and the
// structural invariants verify() re-checks.

#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

const std::vector<fcr_test::PoolSpec> kPools = {
    {fcr::ResourceKind::Rack, "hall-a", 100, 10, 5},
    {fcr::ResourceKind::Power, "feed-a", 1'000'000, 0, 50'000},
    {fcr::ResourceKind::Space, "hall-a", 1'000, 0, 0},
};

/// Checks the library against the reference model and against its own verify().
void check_closure(const fcr::Store& store, const fcr::CapacitySnapshot& snapshot) {
  const fcr::Result<std::vector<fcr::ReservationView>> views = store.list();
  FCR_REQUIRE(views.has_value());
  fcr_test::check_accounting_matches_reference(store, snapshot, fcr_test::reference_from_views(*views));

  const fcr::Result<fcr::VerificationReport> report = store.verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
  FCR_CHECK(report->mismatched_pools.empty());
  FCR_CHECK_EQ(report->pool_count, snapshot.pools.size());
}

}  // namespace

FCR_TEST(ledger_accounting, closure_holds_through_a_full_lifecycle) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  check_closure(fixture.store, fixture.snapshot);

  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1",
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 20),
       fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 200'000)},
      1'100, 50'000)));
  check_closure(fixture.store, fixture.snapshot);

  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-2", "attempt-2", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 30)}, 1'100, 50'000)));
  check_closure(fixture.store, fixture.snapshot);

  const fcr::Result<fcr::ReleaseOutcome> released = fixture.store.release(
      fcr_test::release_request(fixture, "res-1", "attempt-3", fcr::ReservationGeneration(1)));
  FCR_REQUIRE(released.has_value());
  FCR_CHECK_EQ(released->released.size(), std::size_t{2});
  check_closure(fixture.store, fixture.snapshot);

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Rack, "hall-a")->committed, 30ULL);
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Rack, "hall-a")->free, 55ULL);
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Rack, "hall-a")->reservable, 85ULL);
}

FCR_TEST(ledger_accounting, an_amendment_replaces_the_old_claim_set_exactly) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 40)}, 1'100, 50'000)));

  const fcr::Result<fcr::AmendOutcome> amended = fixture.store.amend(fcr_test::amend_request(
      fixture, "res-1", "attempt-2", fcr::ReservationGeneration(1),
      {fcr_test::claim(fcr::ResourceKind::Space, "hall-a", 400)}, 1'100, 60'000));
  FCR_REQUIRE(amended.has_value());
  FCR_CHECK_EQ(amended->previous_generation.value(), 1ULL);
  FCR_CHECK_EQ(amended->reservation.record.generation.value(), 2ULL);
  FCR_CHECK_EQ(amended->reservation.record.lineage.size(), std::size_t{1});

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Rack, "hall-a")->committed, 0ULL);
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Rack, "hall-a")->free, 85ULL);
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Space, "hall-a")->committed, 400ULL);
  check_closure(fixture.store, fixture.snapshot);
}

FCR_TEST(ledger_accounting, an_amendment_that_grows_is_priced_against_free_capacity) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'000)));
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-2", "attempt-2", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 70)}, 1'100, 50'000)));

  // Free is 5; the amendment releases its own 10, so 15 is available and 16 is not.
  FCR_REQUIRE_OK(fixture.store.amend(fcr_test::amend_request(
      fixture, "res-1", "attempt-3", fcr::ReservationGeneration(1),
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 15)}, 1'100, 60'000)));
  check_closure(fixture.store, fixture.snapshot);

  const fcr::Result<fcr::AmendOutcome> too_big = fixture.store.amend(fcr_test::amend_request(
      fixture, "res-1", "attempt-4", fcr::ReservationGeneration(2),
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 16)}, 1'100, 60'000));
  FCR_CHECK_ERROR(too_big, fcr::ErrorCode::InsufficientCapacity);
  check_closure(fixture.store, fixture.snapshot);
}

FCR_TEST(ledger_accounting, an_amendment_that_changes_nothing_is_refused) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'000)));
  FCR_CHECK_ERROR(fixture.store.amend(fcr_test::amend_request(
                      fixture, "res-1", "attempt-2", fcr::ReservationGeneration(1),
                      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'000)),
                  fcr::ErrorCode::AmendmentNoChange);
}

FCR_TEST(ledger_accounting, many_reservations_still_close) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  std::uint64_t reserved_racks = 0;
  std::uint64_t reserved_power = 0;
  for (int index = 0; index < 40; ++index) {
    const std::string id = "res-" + std::to_string(index);
    const std::string attempt_name = "attempt-" + std::to_string(index);
    const std::uint64_t racks = static_cast<std::uint64_t>(index % 3);
    const std::uint64_t power = static_cast<std::uint64_t>(index) * 1'000ULL;
    // A zero claim is not a legal claim, so it is omitted rather than sent.
    std::vector<fcr::ResourceClaim> claims;
    if (racks != 0) {
      claims.push_back(fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", racks));
    }
    if (power != 0) {
      claims.push_back(fcr_test::claim(fcr::ResourceKind::Power, "feed-a", power));
    }
    if (claims.empty()) {
      continue;
    }
    const fcr::Result<fcr::ReserveOutcome> outcome = fixture.store.reserve(
        fcr_test::reserve_request(fixture, id.c_str(), attempt_name.c_str(), claims, 1'100, 50'000));
    FCR_REQUIRE(outcome.has_value());
    reserved_racks += racks;
    reserved_power += power;
  }
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Rack, "hall-a")->committed, reserved_racks);
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Power, "feed-a")->committed, reserved_power);
  check_closure(fixture.store, fixture.snapshot);
}

FCR_TEST(ledger_accounting, mixed_headroom_classes_land_in_the_right_bucket) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::ReserveRequest guaranteed = fcr_test::reserve_request(
      fixture, "res-g", "attempt-g", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 5)}, 1'100, 50'000);
  FCR_REQUIRE_OK(fixture.store.reserve(guaranteed));

  fcr::ReserveRequest firm = fcr_test::reserve_request(
      fixture, "res-f", "attempt-f", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 6)}, 1'100, 50'000);
  firm.headroom = fcr::HeadroomClass::Firm;
  FCR_REQUIRE_OK(fixture.store.reserve(firm));

  fcr::ReserveRequest opportunistic = fcr_test::reserve_request(
      fixture, "res-o", "attempt-o", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 7)}, 1'100, 50'000);
  opportunistic.headroom = fcr::HeadroomClass::Opportunistic;
  FCR_REQUIRE_OK(fixture.store.reserve(opportunistic));

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Rack, "hall-a")->committed, 11ULL);
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Rack, "hall-a")->protected_, 7ULL);
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Rack, "hall-a")->free, 67ULL);
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Rack, "hall-a")->active_reservations, std::size_t{3});
  check_closure(fixture.store, fixture.snapshot);
}

FCR_TEST(ledger_accounting, expiry_returns_capacity_and_closes) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 25)}, 1'100, 2'000)));
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-2", "attempt-2", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 15)}, 1'100, 90'000)));

  const fcr::Result<fcr::ExpireOutcome> expired =
      fixture.store.expire(fcr_test::expire_request(fixture, "attempt-3", 2'000));
  FCR_REQUIRE(expired.has_value());
  FCR_CHECK_EQ(expired->expired.size(), std::size_t{1});
  FCR_CHECK_EQ(expired->expired[0].id.value(), std::string("res-1"));

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Rack, "hall-a")->committed, 15ULL);
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Rack, "hall-a")->free, 70ULL);
  check_closure(fixture.store, fixture.snapshot);

  const fcr::Result<std::optional<fcr::ReservationView>> view = fixture.store.find(fcr_test::reservation_id("res-1"));
  FCR_REQUIRE(view.has_value());
  FCR_REQUIRE(view->has_value());
  FCR_CHECK((*view)->record.state == fcr::ReservationState::Expired);
  FCR_REQUIRE((*view)->record.termination.has_value());
  FCR_CHECK((*view)->record.termination->cause == fcr::TransitionCause::DeadlineElapsed);
  FCR_CHECK((*view)->record.termination->actor.empty());
}

FCR_TEST(ledger_accounting, a_sweep_with_nothing_due_publishes_nothing) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 90'000)));
  const fcr::Result<fcr::Revision> before = fixture.store.revision();
  FCR_REQUIRE(before.has_value());

  const fcr::Result<fcr::ExpireOutcome> expired =
      fixture.store.expire(fcr_test::expire_request(fixture, "attempt-2", 5'000));
  FCR_REQUIRE(expired.has_value());
  FCR_CHECK(expired->expired.empty());
  FCR_CHECK(!expired->replayed);

  const fcr::Result<fcr::Revision> after = fixture.store.revision();
  FCR_REQUIRE(after.has_value());
  FCR_CHECK_EQ(after->value(), before->value());
  const fcr::Result<fcr::LedgerStatus> status = fixture.store.status();
  FCR_REQUIRE(status.has_value());
  FCR_CHECK_EQ(status->attempt_count, std::size_t{1});
}

FCR_TEST(ledger_accounting, the_sweep_bound_is_enforced) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  for (int index = 0; index < 5; ++index) {
    const std::string id = "res-" + std::to_string(index);
    const std::string attempt_name = "attempt-" + std::to_string(index);
    FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
        fixture, id.c_str(), attempt_name.c_str(), {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100,
        2'000)));
  }
  fcr::ExpireRequest request = fcr_test::expire_request(fixture, "attempt-sweep", 5'000);
  request.limits.max_expirations_per_sweep = 2;
  FCR_CHECK_ERROR(fixture.store.expire(request), fcr::ErrorCode::LimitExceeded);
  check_closure(fixture.store, fixture.snapshot);
}

FCR_TEST(ledger_accounting, the_reservation_bound_is_enforced) {
  fcr::StoreOptions options;
  options.ledger.max_reservations = 2;
  fcr::Result<fcr::Store> opened = fcr::Store::in_memory(options);
  FCR_REQUIRE(opened.has_value());
  fcr::Store& store = opened.value();
  FCR_REQUIRE_OK(store.install_capacity(fcr_test::make_snapshot("snap", "site", 1, 100, kPools), fcr::Tick(1'000)));
  fcr_test::Fixture fixture;
  fixture.store = fcr::Store::in_memory().value();
  fixture.snapshot = fcr_test::make_snapshot("snap", "site", 1, 100, kPools);
  fixture.epoch = store.epoch().value();

  FCR_REQUIRE_OK(store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 50'000)));
  FCR_REQUIRE_OK(store.reserve(fcr_test::reserve_request(
      fixture, "res-2", "attempt-2", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 50'000)));
  FCR_CHECK_ERROR(store.reserve(fcr_test::reserve_request(
                      fixture, "res-3", "attempt-3", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100,
                      50'000)),
                  fcr::ErrorCode::LimitExceeded);
}

FCR_TEST(ledger_accounting, the_attempt_index_is_bounded_and_evicts_oldest_first) {
  fcr::StoreOptions options;
  options.ledger.max_attempts = 3;
  fcr::Result<fcr::Store> opened = fcr::Store::in_memory(options);
  FCR_REQUIRE(opened.has_value());
  fcr::Store& store = opened.value();
  FCR_REQUIRE_OK(store.install_capacity(fcr_test::make_snapshot("snap", "site", 1, 100, kPools), fcr::Tick(1'000)));

  fcr_test::Fixture fixture;
  fixture.snapshot = fcr_test::make_snapshot("snap", "site", 1, 100, kPools);
  fixture.epoch = store.epoch().value();

  fcr::ReserveRequest first = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 50'000);
  FCR_REQUIRE_OK(store.reserve(first));
  for (int index = 0; index < 5; ++index) {
    const std::string id = "res-filler-" + std::to_string(index);
    const std::string attempt_name = "attempt-filler-" + std::to_string(index);
    FCR_REQUIRE_OK(store.reserve(fcr_test::reserve_request(
        fixture, id.c_str(), attempt_name.c_str(), {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100,
        50'000)));
  }
  const fcr::Result<fcr::LedgerStatus> status = store.status();
  FCR_REQUIRE(status.has_value());
  FCR_CHECK_EQ(status->attempt_count, std::size_t{3});

  // The evicted attempt is no longer replayable, and re-applying it is refused
  // rather than silently reserving again.
  FCR_CHECK_ERROR(store.reserve(first), fcr::ErrorCode::ReservationAlreadyExists);
  check_closure(store, fixture.snapshot);
}

FCR_TEST(ledger_accounting, verify_recomputes_and_agrees) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  for (int index = 0; index < 12; ++index) {
    const std::string id = "res-" + std::to_string(index);
    const std::string attempt_name = "attempt-" + std::to_string(index);
    fcr::ReserveRequest request = fcr_test::reserve_request(
        fixture, id.c_str(), attempt_name.c_str(), {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 3)}, 1'100,
        50'000);
    if (index % 2 == 0) {
      request.headroom = fcr::HeadroomClass::Opportunistic;
    }
    FCR_REQUIRE_OK(fixture.store.reserve(request));
  }
  const fcr::Result<fcr::VerificationReport> report = fixture.store.verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
  FCR_CHECK_EQ(report->reservation_count, std::size_t{12});
  FCR_CHECK_EQ(report->active_count, std::size_t{12});
  FCR_CHECK_EQ(report->attempt_count, std::size_t{12});
  FCR_CHECK_EQ(report->pool_count, std::size_t{3});
  FCR_CHECK(report->mismatched_pools.empty());
}

FCR_TEST(ledger_accounting, accounting_of_reports_an_unknown_pool) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::PoolKey unknown{fcr::ResourceKind::Rack, fcr_test::scope("hall-z")};
  FCR_CHECK_ERROR(fixture.store.accounting_of(unknown), fcr::ErrorCode::PoolUnknown);
  const fcr::PoolKey known{fcr::ResourceKind::Rack, fcr_test::scope("hall-a")};
  const fcr::Result<fcr::CapacitySplit> split = fixture.store.accounting_of(known);
  FCR_REQUIRE(split.has_value());
  FCR_CHECK_EQ(split->free.units(), 85ULL);
  FCR_CHECK_EQ(split->accounted().value().units(), 85ULL);
}
