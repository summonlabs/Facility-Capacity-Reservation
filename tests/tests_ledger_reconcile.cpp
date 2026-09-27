// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Reconciliation: adopting a newer capacity snapshot, detecting over-commit,
// deterministic fencing order, the guaranteed-commitment boundary, and the
// freshness rules that separate capacity from authority to consume it.

#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

const std::vector<fcr_test::PoolSpec> kPools = {
    {fcr::ResourceKind::Rack, "hall-a", 40, 0, 0},
    {fcr::ResourceKind::Power, "feed-a", 400'000, 0, 0},
};

fcr::Result<fcr::ReserveOutcome> reserve_with(fcr_test::Fixture& fixture, const char* id, const char* attempt_name,
                                              std::uint64_t racks, fcr::HeadroomClass headroom,
                                              std::uint64_t deadline) {
  fcr::ReserveRequest request = fcr_test::reserve_request(
      fixture, id, attempt_name, {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", racks)}, 1'100, deadline);
  request.headroom = headroom;
  return fixture.store.reserve(request);
}

fcr::CapacitySnapshot grown(std::uint64_t generation, std::uint64_t racks) {
  return fcr_test::make_snapshot("snap-next", "site-fixture", generation, generation * 1'000,
                                 {{fcr::ResourceKind::Rack, "hall-a", racks, 0, 0},
                                  {fcr::ResourceKind::Power, "feed-a", 400'000, 0, 0}});
}

}  // namespace

FCR_TEST(ledger_reconcile, adopting_a_newer_snapshot_publishes_one_revision) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::Revision> before = fixture.store.revision();
  FCR_REQUIRE(before.has_value());

  const fcr::CapacitySnapshot next = grown(2, 60);
  const fcr::Result<fcr::ReconcileOutcome> outcome = fixture.store.reconcile(
      fcr_test::reconcile_request(fixture, "reconcile-1", next, fcr::SourceGeneration(1), fcr::ReconcileMode::Enforce));
  FCR_REQUIRE(outcome.has_value());
  FCR_CHECK(outcome->adopted);
  FCR_CHECK(!outcome->replayed);
  FCR_CHECK(outcome->overcommits.empty());
  FCR_CHECK(outcome->fenced.empty());
  FCR_CHECK_EQ(outcome->previous_source_generation.value(), 1ULL);
  FCR_CHECK_EQ(outcome->source_generation.value(), 2ULL);
  FCR_CHECK_EQ(outcome->revision.value(), before->value() + 1);

  const fcr::Result<fcr::LedgerStatus> status = fixture.store.status();
  FCR_REQUIRE(status.has_value());
  FCR_CHECK(status->capacity_fresh);
  FCR_CHECK_EQ(status->source_generation.value(), 2ULL);
}

FCR_TEST(ledger_reconcile, the_source_generation_precondition_is_exact) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::CapacitySnapshot next = grown(2, 60);
  FCR_CHECK_ERROR(fixture.store.reconcile(fcr_test::reconcile_request(
                      fixture, "reconcile-1", next, fcr::SourceGeneration(7), fcr::ReconcileMode::Enforce)),
                  fcr::ErrorCode::SourceGenerationStale);
  FCR_CHECK_ERROR(fixture.store.reconcile(fcr_test::reconcile_request(
                      fixture, "reconcile-2", next, fcr::SourceGeneration(0), fcr::ReconcileMode::Enforce)),
                  fcr::ErrorCode::MissingField);
}

FCR_TEST(ledger_reconcile, a_snapshot_that_is_not_newer_is_refused) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_CHECK_ERROR(fixture.store.reconcile(fcr_test::reconcile_request(
                      fixture, "reconcile-1", grown(1, 60), fcr::SourceGeneration(1), fcr::ReconcileMode::Enforce)),
                  fcr::ErrorCode::SourceGenerationConflict);
  // A snapshot whose source generation is zero is not a malformed document: it
  // is one that is missing the generation that gives it meaning.
  FCR_CHECK_ERROR(fixture.store.reconcile(fcr_test::reconcile_request(
                      fixture, "reconcile-2", grown(0, 60), fcr::SourceGeneration(1), fcr::ReconcileMode::Enforce)),
                  fcr::ErrorCode::MissingField);
}

FCR_TEST(ledger_reconcile, observation_reports_the_overcommit_and_changes_nothing) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE(reserve_with(fixture, "res-a", "attempt-1", 20, fcr::HeadroomClass::Guaranteed, 900'000).has_value());
  FCR_REQUIRE(reserve_with(fixture, "res-b", "attempt-2", 15, fcr::HeadroomClass::Firm, 800'000).has_value());

  const fcr::Result<fcr::Revision> before = fixture.store.revision();
  FCR_REQUIRE(before.has_value());

  const fcr::Result<fcr::ReconcileOutcome> outcome = fixture.store.reconcile(
      fcr_test::reconcile_request(fixture, "observe-1", grown(2, 30), fcr::SourceGeneration(1),
                                  fcr::ReconcileMode::Observe));
  FCR_REQUIRE(outcome.has_value());
  FCR_CHECK(!outcome->adopted);
  FCR_CHECK(outcome->fenced.empty());
  FCR_REQUIRE(outcome->overcommits.size() == 1);
  FCR_CHECK_EQ(outcome->overcommits[0].reservable, 30ULL);
  FCR_CHECK_EQ(outcome->overcommits[0].committed, 35ULL);
  FCR_CHECK_EQ(outcome->overcommits[0].excess, 5ULL);

  const fcr::Result<fcr::Revision> after = fixture.store.revision();
  FCR_REQUIRE(after.has_value());
  FCR_CHECK_EQ(after->value(), before->value());
  const fcr::Result<fcr::LedgerStatus> status = fixture.store.status();
  FCR_REQUIRE(status.has_value());
  FCR_CHECK_EQ(status->source_generation.value(), 1ULL);
  FCR_CHECK_EQ(status->attempt_count, std::size_t{2});
}

FCR_TEST(ledger_reconcile, enforcement_fences_opportunistic_before_firm) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE(reserve_with(fixture, "res-guaranteed", "attempt-1", 10, fcr::HeadroomClass::Guaranteed, 900'000)
                  .has_value());
  FCR_REQUIRE(reserve_with(fixture, "res-firm", "attempt-2", 10, fcr::HeadroomClass::Firm, 800'000).has_value());
  FCR_REQUIRE(
      reserve_with(fixture, "res-opportunistic", "attempt-3", 10, fcr::HeadroomClass::Opportunistic, 700'000)
          .has_value());

  // 30 units are held (20 committed + 10 protected); 25 remain reservable.
  const fcr::Result<fcr::ReconcileOutcome> outcome = fixture.store.reconcile(
      fcr_test::reconcile_request(fixture, "enforce-1", grown(2, 25), fcr::SourceGeneration(1),
                                  fcr::ReconcileMode::Enforce));
  FCR_REQUIRE(outcome.has_value());
  FCR_CHECK(outcome->adopted);
  FCR_REQUIRE(outcome->fenced.size() == 1);
  FCR_CHECK_EQ(outcome->fenced[0].id.value(), std::string("res-opportunistic"));
  FCR_CHECK(outcome->fenced[0].headroom == fcr::HeadroomClass::Opportunistic);

  const fcr::Result<std::optional<fcr::ReservationView>> fenced =
      fixture.store.find(fcr_test::reservation_id("res-opportunistic"));
  FCR_REQUIRE(fenced.has_value());
  FCR_REQUIRE(fenced->has_value());
  FCR_CHECK((*fenced)->record.state == fcr::ReservationState::Revoked);
  FCR_REQUIRE((*fenced)->record.termination.has_value());
  FCR_CHECK((*fenced)->record.termination->cause == fcr::TransitionCause::CapacityWithdrawn);
  FCR_CHECK_EQ((*fenced)->record.termination->actor.value(), std::string("facility-authority"));

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ((*pools)[0].reservable, 25ULL);
  FCR_CHECK_EQ((*pools)[0].committed, 20ULL);
  FCR_CHECK_EQ((*pools)[0].protected_, 0ULL);
  FCR_CHECK_EQ((*pools)[0].free, 5ULL);
}

FCR_TEST(ledger_reconcile, enforcement_fences_the_earliest_deadline_first_within_a_class) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE(reserve_with(fixture, "res-guaranteed", "attempt-1", 20, fcr::HeadroomClass::Guaranteed, 900'000)
                  .has_value());
  FCR_REQUIRE(reserve_with(fixture, "res-firm-late", "attempt-2", 5, fcr::HeadroomClass::Firm, 800'000).has_value());
  FCR_REQUIRE(reserve_with(fixture, "res-firm-early", "attempt-3", 5, fcr::HeadroomClass::Firm, 400'000).has_value());

  const fcr::Result<fcr::ReconcileOutcome> outcome = fixture.store.reconcile(
      fcr_test::reconcile_request(fixture, "enforce-1", grown(2, 25), fcr::SourceGeneration(1),
                                  fcr::ReconcileMode::Enforce));
  FCR_REQUIRE(outcome.has_value());
  FCR_REQUIRE(outcome->fenced.size() == 1);
  FCR_CHECK_EQ(outcome->fenced[0].id.value(), std::string("res-firm-early"));
}

FCR_TEST(ledger_reconcile, an_overcommit_that_survives_guaranteed_commitments_is_unresolvable) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE(reserve_with(fixture, "res-a", "attempt-1", 20, fcr::HeadroomClass::Guaranteed, 900'000).has_value());
  FCR_REQUIRE(reserve_with(fixture, "res-b", "attempt-2", 15, fcr::HeadroomClass::Guaranteed, 900'000).has_value());
  const fcr::Result<fcr::Revision> before = fixture.store.revision();
  FCR_REQUIRE(before.has_value());

  const fcr::Result<fcr::ReconcileOutcome> outcome = fixture.store.reconcile(
      fcr_test::reconcile_request(fixture, "enforce-1", grown(2, 30), fcr::SourceGeneration(1),
                                  fcr::ReconcileMode::Enforce));
  FCR_CHECK_ERROR(outcome, fcr::ErrorCode::UnresolvableOvercommit);

  // Nothing was adopted and nothing was fenced: a reconciliation either closes
  // the accounting or changes nothing at all.
  const fcr::Result<fcr::Revision> after = fixture.store.revision();
  FCR_REQUIRE(after.has_value());
  FCR_CHECK_EQ(after->value(), before->value());
  const fcr::Result<fcr::LedgerStatus> status = fixture.store.status();
  FCR_REQUIRE(status.has_value());
  FCR_CHECK_EQ(status->source_generation.value(), 1ULL);
  FCR_CHECK_EQ(status->active_count, std::size_t{2});
}

FCR_TEST(ledger_reconcile, enforcement_requires_a_policy_when_it_would_fence) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE(reserve_with(fixture, "res-a", "attempt-1", 30, fcr::HeadroomClass::Firm, 900'000).has_value());
  fcr::ReconcileRequest request = fcr_test::reconcile_request(
      fixture, "enforce-1", grown(2, 20), fcr::SourceGeneration(1), fcr::ReconcileMode::Enforce);
  request.policy.reset();
  FCR_CHECK_ERROR(fixture.store.reconcile(request), fcr::ErrorCode::PolicyOverrideRequired);
}

FCR_TEST(ledger_reconcile, a_pool_that_disappears_fences_its_claimants) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE(reserve_with(fixture, "res-a", "attempt-1", 10, fcr::HeadroomClass::Firm, 900'000).has_value());
  const fcr::CapacitySnapshot without_racks = fcr_test::make_snapshot(
      "snap-next", "site-fixture", 2, 2'000, {{fcr::ResourceKind::Power, "feed-a", 400'000, 0, 0}});

  const fcr::Result<fcr::ReconcileOutcome> outcome = fixture.store.reconcile(fcr_test::reconcile_request(
      fixture, "enforce-1", without_racks, fcr::SourceGeneration(1), fcr::ReconcileMode::Enforce));
  FCR_REQUIRE(outcome.has_value());
  FCR_REQUIRE(outcome->fenced.size() == 1);
  FCR_CHECK_EQ(outcome->fenced[0].id.value(), std::string("res-a"));

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ(pools->size(), std::size_t{1});
  FCR_CHECK((*pools)[0].pool.kind == fcr::ResourceKind::Power);
}

FCR_TEST(ledger_reconcile, survivors_are_reported_stale_until_their_claimant_re_prices_them) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE(reserve_with(fixture, "res-a", "attempt-1", 10, fcr::HeadroomClass::Guaranteed, 900'000).has_value());
  FCR_REQUIRE_OK(fixture.store.reconcile(fcr_test::reconcile_request(
      fixture, "reconcile-1", grown(2, 60), fcr::SourceGeneration(1), fcr::ReconcileMode::Enforce)));

  const fcr::Result<std::optional<fcr::ReservationView>> view = fixture.store.find(fcr_test::reservation_id("res-a"));
  FCR_REQUIRE(view.has_value());
  FCR_REQUIRE(view->has_value());
  FCR_CHECK((*view)->source_stale);
  FCR_CHECK_EQ((*view)->record.source_generation.value(), 1ULL);

  // Consuming operations still refuse the older generation, so the claimant must
  // re-price the commitment explicitly.
  FCR_CHECK_ERROR(fixture.store.amend(fcr_test::amend_request(
                      fixture, "res-a", "attempt-2", fcr::ReservationGeneration(1),
                      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 11)}, 1'100, 900'000)),
                  fcr::ErrorCode::SourceGenerationStale);
}

FCR_TEST(ledger_reconcile, a_repeated_reconciliation_replays) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE(reserve_with(fixture, "res-a", "attempt-1", 30, fcr::HeadroomClass::Firm, 900'000).has_value());
  const fcr::ReconcileRequest request = fcr_test::reconcile_request(
      fixture, "enforce-1", grown(2, 20), fcr::SourceGeneration(1), fcr::ReconcileMode::Enforce);

  const fcr::Result<fcr::ReconcileOutcome> first = fixture.store.reconcile(request);
  FCR_REQUIRE(first.has_value());
  FCR_CHECK(first->adopted);
  FCR_REQUIRE(first->fenced.size() == 1);

  const fcr::Result<fcr::ReconcileOutcome> second = fixture.store.reconcile(request);
  FCR_REQUIRE(second.has_value());
  FCR_CHECK(second->replayed);
  FCR_CHECK_EQ(second->revision.value(), first->revision.value());
  FCR_REQUIRE(second->fenced.size() == 1);
  FCR_CHECK_EQ(second->fenced[0].id.value(), first->fenced[0].id.value());
}

FCR_TEST(ledger_reconcile, reconcile_before_any_capacity_is_installed_is_refused) {
  fcr::Result<fcr::Store> opened = fcr::Store::in_memory();
  FCR_REQUIRE(opened.has_value());
  fcr::Store& store = opened.value();
  const fcr::Result<fcr::AuthorityEpoch> epoch = store.epoch();
  FCR_REQUIRE(epoch.has_value());
  fcr_test::Fixture fixture;
  fixture.epoch = *epoch;
  FCR_CHECK_ERROR(store.reconcile(fcr_test::reconcile_request(
                      fixture, "reconcile-1", grown(2, 60), fcr::SourceGeneration(1), fcr::ReconcileMode::Enforce)),
                  fcr::ErrorCode::NoCapacityInstalled);
}

FCR_TEST(ledger_reconcile, fencing_reaches_closure_across_several_pools) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE(reserve_with(fixture, "res-racks", "attempt-1", 30, fcr::HeadroomClass::Firm, 900'000).has_value());
  fcr::ReserveRequest power = fcr_test::reserve_request(
      fixture, "res-power", "attempt-2", {fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 300'000)}, 1'100,
      900'000);
  power.headroom = fcr::HeadroomClass::Firm;
  FCR_REQUIRE_OK(fixture.store.reserve(power));

  const fcr::CapacitySnapshot reduced = fcr_test::make_snapshot(
      "snap-next", "site-fixture", 2, 2'000,
      {{fcr::ResourceKind::Rack, "hall-a", 20, 0, 0}, {fcr::ResourceKind::Power, "feed-a", 200'000, 0, 0}});
  const fcr::Result<fcr::ReconcileOutcome> outcome = fixture.store.reconcile(fcr_test::reconcile_request(
      fixture, "enforce-1", reduced, fcr::SourceGeneration(1), fcr::ReconcileMode::Enforce));
  FCR_REQUIRE(outcome.has_value());
  FCR_CHECK_EQ(outcome->fenced.size(), std::size_t{2});

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  for (const fcr::PoolAccount& account : *pools) {
    FCR_CHECK_EQ(account.committed, 0ULL);
    FCR_CHECK_EQ(account.protected_, 0ULL);
    FCR_CHECK_EQ(account.free, account.reservable);
  }
  const fcr::Result<fcr::VerificationReport> report = fixture.store.verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
}

FCR_TEST(ledger_reconcile, an_empty_pool_snapshot_is_refused) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::CapacitySnapshot empty = grown(2, 60);
  empty.pools.clear();
  FCR_CHECK_ERROR(fixture.store.reconcile(fcr_test::reconcile_request(
                      fixture, "reconcile-1", empty, fcr::SourceGeneration(1), fcr::ReconcileMode::Enforce)),
                  fcr::ErrorCode::SnapshotEmpty);
}

FCR_TEST(ledger_reconcile, an_unsorted_snapshot_is_refused) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::CapacitySnapshot unsorted = grown(2, 60);
  std::swap(unsorted.pools[0], unsorted.pools[1]);
  FCR_CHECK_ERROR(fixture.store.reconcile(fcr_test::reconcile_request(
                      fixture, "reconcile-1", unsorted, fcr::SourceGeneration(1), fcr::ReconcileMode::Enforce)),
                  fcr::ErrorCode::NonCanonicalOrder);
}

FCR_TEST(ledger_reconcile, a_stale_epoch_cannot_reconcile) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::ReconcileRequest request = fcr_test::reconcile_request(
      fixture, "reconcile-1", grown(2, 60), fcr::SourceGeneration(1), fcr::ReconcileMode::Enforce);
  request.authority.epoch = fcr::AuthorityEpoch(fixture.epoch.value() + 1);
  FCR_CHECK_ERROR(fixture.store.reconcile(request), fcr::ErrorCode::StaleAuthorityEpoch);
}
