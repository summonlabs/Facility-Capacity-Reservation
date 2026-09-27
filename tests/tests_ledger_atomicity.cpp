// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Atomicity: a multi-resource commitment is applied in full or not at all, a
// failed operation leaves the ledger byte-for-byte unchanged, and no partial
// state is ever observable.

#include <string>
#include <vector>

#include "dccp/facility_capacity_reservation/digest.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

const std::vector<fcr_test::PoolSpec> kPools = {
    {fcr::ResourceKind::Rack, "hall-a", 20, 0, 0},
    {fcr::ResourceKind::Power, "feed-a", 100'000, 0, 0},
    {fcr::ResourceKind::Cooling, "loop-a", 90'000, 0, 0},
    {fcr::ResourceKind::Space, "hall-a", 200, 0, 0},
};

/// A digest of everything a caller can observe about a store.
std::string observable_digest(const fcr::Store& store) {
  std::string material;
  const fcr::Result<fcr::LedgerStatus> status = store.status();
  if (!status.has_value()) {
    return "status-error";
  }
  material.append(fcr::format_unsigned(status->revision.value()));
  material.push_back('|');
  material.append(fcr::format_unsigned(status->reservation_count));
  material.push_back('|');
  material.append(fcr::format_unsigned(status->attempt_count));
  material.push_back('|');
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
  const fcr::Result<std::vector<fcr::ReservationView>> views = store.list();
  if (views.has_value()) {
    for (const fcr::ReservationView& view : *views) {
      material.append(view.record.id.value());
      material.push_back(':');
      material.append(fcr::format_unsigned(view.record.generation.value()));
      material.push_back(':');
      material.append(fcr::reservation_state_token(view.record.state));
      material.push_back(';');
    }
  }
  return fcr::sha256_hex(material);
}

}  // namespace

FCR_TEST(ledger_atomicity, a_multi_resource_commit_is_all_or_nothing) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const std::string before = observable_digest(fixture.store);

  // The rack and power amounts fit; the cooling amount does not. Nothing may be
  // bound, and the failure must name the pool that refused.
  const fcr::Result<fcr::ReserveOutcome> outcome = fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1",
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 5),
       fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 10'000),
       fcr_test::claim(fcr::ResourceKind::Cooling, "loop-a", 90'001)},
      1'100, 50'000));
  FCR_REQUIRE(!outcome.has_value());
  FCR_CHECK(outcome.error().code() == fcr::ErrorCode::InsufficientCapacity);
  FCR_CHECK(outcome.error().subject().find("cooling:loop-a") != std::string::npos);

  FCR_CHECK_EQ(observable_digest(fixture.store), before);
  const fcr::Result<std::optional<fcr::ReservationView>> missing =
      fixture.store.find(fcr_test::reservation_id("res-1"));
  FCR_REQUIRE(missing.has_value());
  FCR_CHECK(!missing->has_value());
}

FCR_TEST(ledger_atomicity, a_failed_reserve_consumes_no_attempt_identity) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::ReserveRequest request = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 999)}, 1'100, 50'000);
  FCR_CHECK_ERROR(fixture.store.reserve(request), fcr::ErrorCode::InsufficientCapacity);

  const fcr::Result<fcr::LedgerStatus> status = fixture.store.status();
  FCR_REQUIRE(status.has_value());
  FCR_CHECK_EQ(status->attempt_count, std::size_t{0});
  FCR_CHECK_EQ(status->revision.value(), 1ULL);

  // The same attempt identity may therefore be used for a corrected request.
  request.claims = {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)};
  FCR_REQUIRE_OK(fixture.store.reserve(request));
}

FCR_TEST(ledger_atomicity, a_failed_amendment_leaves_the_original_binding_intact) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1",
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10),
       fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 50'000)},
      1'100, 50'000)));
  const std::string before = observable_digest(fixture.store);

  // The rack amount fits after releasing the old claim (10 free + 10 own = 20),
  // but the cooling claim cannot be satisfied at all.
  FCR_CHECK_ERROR(fixture.store.amend(fcr_test::amend_request(
                      fixture, "res-1", "attempt-2", fcr::ReservationGeneration(1),
                      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 20),
                       fcr_test::claim(fcr::ResourceKind::Cooling, "loop-a", 200'000)},
                      1'100, 60'000)),
                  fcr::ErrorCode::InsufficientCapacity);
  FCR_CHECK_EQ(observable_digest(fixture.store), before);

  const fcr::Result<std::optional<fcr::ReservationView>> view = fixture.store.find(fcr_test::reservation_id("res-1"));
  FCR_REQUIRE(view.has_value());
  FCR_REQUIRE(view->has_value());
  FCR_CHECK_EQ((*view)->record.generation.value(), 1ULL);
  FCR_CHECK_EQ((*view)->record.claims.size(), std::size_t{2});
  FCR_CHECK((*view)->record.lineage.empty());
}

FCR_TEST(ledger_atomicity, a_failed_release_leaves_the_reservation_active) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'000)));
  const std::string before = observable_digest(fixture.store);

  fcr::ReleaseRequest wrong_actor = fcr_test::release_request(fixture, "res-1", "attempt-2",
                                                              fcr::ReservationGeneration(1));
  wrong_actor.actor = fcr_test::actor("someone-else");
  FCR_CHECK_ERROR(fixture.store.release(wrong_actor), fcr::ErrorCode::ClaimantMismatch);
  FCR_CHECK_EQ(observable_digest(fixture.store), before);

  fcr::ReleaseRequest wrong_generation = fcr_test::release_request(fixture, "res-1", "attempt-3",
                                                                   fcr::ReservationGeneration(9));
  FCR_CHECK_ERROR(fixture.store.release(wrong_generation), fcr::ErrorCode::StaleReservationGeneration);
  FCR_CHECK_EQ(observable_digest(fixture.store), before);
}

FCR_TEST(ledger_atomicity, a_second_identical_request_is_the_only_way_to_repeat) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::ReserveRequest request = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 4)}, 1'100, 50'000);
  FCR_REQUIRE_OK(fixture.store.reserve(request));
  const std::string after_first = observable_digest(fixture.store);

  const fcr::Result<fcr::ReserveOutcome> replay = fixture.store.reserve(request);
  FCR_REQUIRE(replay.has_value());
  FCR_CHECK(replay->replayed);
  FCR_CHECK_EQ(observable_digest(fixture.store), after_first);
}

FCR_TEST(ledger_atomicity, closure_is_reestablished_after_every_rejection) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  for (int index = 0; index < 30; ++index) {
    const std::string id = "res-" + std::to_string(index);
    const std::string attempt_name = "attempt-" + std::to_string(index);
    // Deliberately include amounts that cannot fit so that roughly half of the
    // attempts are rejected.
    const std::uint64_t amount = static_cast<std::uint64_t>(1 + (index * 7) % 25);
    (void)fixture.store.reserve(fcr_test::reserve_request(
        fixture, id.c_str(), attempt_name.c_str(), {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", amount)},
        1'100, 50'000));

    const fcr::Result<fcr::VerificationReport> report = fixture.store.verify();
    FCR_REQUIRE(report.has_value());
    FCR_CHECK(report->ok);

    const fcr::Result<std::vector<fcr::ReservationView>> views = fixture.store.list();
    FCR_REQUIRE(views.has_value());
    fcr_test::check_accounting_matches_reference(fixture.store, fixture.snapshot,
                                                 fcr_test::reference_from_views(*views));
  }
}

FCR_TEST(ledger_atomicity, an_amendment_reserves_its_own_capacity_before_pricing_new_claims) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 20)}, 1'100, 50'000)));

  // The whole pool is held by one reservation; reshaping it in place must work
  // even though free capacity is zero.
  FCR_REQUIRE_OK(fixture.store.amend(fcr_test::amend_request(
      fixture, "res-1", "attempt-2", fcr::ReservationGeneration(1),
      {fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 100'000)}, 1'100, 60'000)));

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Rack, "hall-a")->committed, 0ULL);
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Power, "feed-a")->committed, 100'000ULL);
  FCR_CHECK_EQ(fcr_test::find_pool(*pools, fcr::ResourceKind::Power, "feed-a")->free, 0ULL);
}

FCR_TEST(ledger_atomicity, a_rejected_reserve_does_not_leak_a_revision) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::Revision> before = fixture.store.revision();
  FCR_REQUIRE(before.has_value());
  for (int index = 0; index < 10; ++index) {
    const std::string attempt_name = "attempt-" + std::to_string(index);
    FCR_CHECK_ERROR(
        fixture.store.reserve(fcr_test::reserve_request(
            fixture, "res-big", attempt_name.c_str(), {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1'000)},
            1'100, 50'000)),
        fcr::ErrorCode::InsufficientCapacity);
  }
  const fcr::Result<fcr::Revision> after = fixture.store.revision();
  FCR_REQUIRE(after.has_value());
  FCR_CHECK_EQ(after->value(), before->value());
}

FCR_TEST(ledger_atomicity, an_exact_fit_across_every_pool_is_accepted) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> outcome = fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1",
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 20),
       fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 100'000),
       fcr_test::claim(fcr::ResourceKind::Cooling, "loop-a", 90'000),
       fcr_test::claim(fcr::ResourceKind::Space, "hall-a", 200)},
      1'100, 50'000));
  FCR_REQUIRE(outcome.has_value());
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  for (const fcr::PoolAccount& account : *pools) {
    FCR_CHECK_EQ(account.free, 0ULL);
    FCR_CHECK_EQ(account.committed, account.reservable);
  }
}

FCR_TEST(ledger_atomicity, one_unit_over_an_exact_fit_is_refused_in_every_pool) {
  for (const fcr::ResourceKind kind : {fcr::ResourceKind::Rack, fcr::ResourceKind::Power, fcr::ResourceKind::Cooling,
                                       fcr::ResourceKind::Space}) {
    fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
    std::vector<fcr::ResourceClaim> claims = {
        fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 20),
        fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 100'000),
        fcr_test::claim(fcr::ResourceKind::Cooling, "loop-a", 90'000),
        fcr_test::claim(fcr::ResourceKind::Space, "hall-a", 200),
    };
    for (fcr::ResourceClaim& item : claims) {
      if (item.pool.kind == kind) {
        item.amount += 1;
      }
    }
    FCR_CHECK_ERROR(
        fixture.store.reserve(fcr_test::reserve_request(fixture, "res-1", "attempt-1", claims, 1'100, 50'000)),
        fcr::ErrorCode::InsufficientCapacity);
    const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
    FCR_REQUIRE(pools.has_value());
    for (const fcr::PoolAccount& account : *pools) {
      FCR_CHECK_EQ(account.committed, 0ULL);
      FCR_CHECK_EQ(account.free, account.reservable);
    }
  }
}
