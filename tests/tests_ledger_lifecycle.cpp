// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Lifecycle: release, expiry, revocation, amendment lineage, terminal-state
// rules and provenance.

#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

const std::vector<fcr_test::PoolSpec> kPools = {
    {fcr::ResourceKind::Rack, "hall-a", 50, 0, 0},
    {fcr::ResourceKind::Power, "feed-a", 500'000, 0, 0},
};

fcr::Result<fcr::ReserveOutcome> reserve_one(fcr_test::Fixture& fixture, const char* id, const char* attempt_name,
                                             std::uint64_t racks, std::uint64_t deadline,
                                             fcr::HeadroomClass headroom = fcr::HeadroomClass::Guaranteed) {
  fcr::ReserveRequest request = fcr_test::reserve_request(
      fixture, id, attempt_name, {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", racks)}, 1'100, deadline);
  request.headroom = headroom;
  return fixture.store.reserve(request);
}

}  // namespace

FCR_TEST(ledger_lifecycle, release_records_full_provenance) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = reserve_one(fixture, "res-1", "attempt-1", 10, 50'000);
  FCR_REQUIRE(reserved.has_value());

  fcr::ReleaseRequest request = fcr_test::release_request(fixture, "res-1", "attempt-2",
                                                          reserved->reservation.record.generation);
  request.detail = "workload finished";
  request.now = fcr::Tick(4'000);
  const fcr::Result<fcr::ReleaseOutcome> released = fixture.store.release(request);
  FCR_REQUIRE(released.has_value());
  FCR_CHECK(released->reservation.record.state == fcr::ReservationState::Released);
  FCR_REQUIRE(released->reservation.record.termination.has_value());
  const fcr::TerminationProvenance& provenance = *released->reservation.record.termination;
  FCR_CHECK(provenance.cause == fcr::TransitionCause::ClaimantRequest);
  FCR_CHECK_EQ(provenance.actor.value(), std::string("claimant-a"));
  FCR_CHECK_EQ(provenance.attempt.value(), std::string("attempt-2"));
  FCR_CHECK_EQ(provenance.epoch.value(), fixture.epoch.value());
  FCR_CHECK_EQ(provenance.at_tick.value(), 4'000ULL);
  FCR_CHECK_EQ(provenance.detail, std::string("workload finished"));
  FCR_CHECK(!provenance.policy.has_value());
  FCR_CHECK_EQ(provenance.revision.value(), released->revision.value());
}

FCR_TEST(ledger_lifecycle, a_released_reservation_cannot_be_released_again_with_a_new_attempt) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = reserve_one(fixture, "res-1", "attempt-1", 10, 50'000);
  FCR_REQUIRE(reserved.has_value());
  FCR_REQUIRE_OK(fixture.store.release(fcr_test::release_request(fixture, "res-1", "attempt-2",
                                                                 reserved->reservation.record.generation)));
  FCR_CHECK_ERROR(fixture.store.release(fcr_test::release_request(fixture, "res-1", "attempt-3",
                                                                  reserved->reservation.record.generation)),
                  fcr::ErrorCode::ReservationTerminal);
}

FCR_TEST(ledger_lifecycle, a_terminal_reservation_cannot_be_amended) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = reserve_one(fixture, "res-1", "attempt-1", 10, 50'000);
  FCR_REQUIRE(reserved.has_value());
  FCR_REQUIRE_OK(fixture.store.release(fcr_test::release_request(fixture, "res-1", "attempt-2",
                                                                 reserved->reservation.record.generation)));
  FCR_CHECK_ERROR(fixture.store.amend(fcr_test::amend_request(
                      fixture, "res-1", "attempt-3", reserved->reservation.record.generation,
                      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 12)}, 1'100, 60'000)),
                  fcr::ErrorCode::ReservationTerminal);
}

FCR_TEST(ledger_lifecycle, only_the_claimant_may_amend_or_release) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = reserve_one(fixture, "res-1", "attempt-1", 10, 50'000);
  FCR_REQUIRE(reserved.has_value());

  fcr::AmendRequest amend = fcr_test::amend_request(
      fixture, "res-1", "attempt-2", reserved->reservation.record.generation,
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 12)}, 1'100, 60'000);
  amend.actor = fcr_test::actor("someone-else");
  FCR_CHECK_ERROR(fixture.store.amend(amend), fcr::ErrorCode::ClaimantMismatch);

  fcr::ReleaseRequest release = fcr_test::release_request(fixture, "res-1", "attempt-3",
                                                          reserved->reservation.record.generation);
  release.actor = fcr_test::actor("someone-else");
  FCR_CHECK_ERROR(fixture.store.release(release), fcr::ErrorCode::ClaimantMismatch);
}

FCR_TEST(ledger_lifecycle, revocation_requires_a_policy_and_an_authority_actor) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved =
      reserve_one(fixture, "res-1", "attempt-1", 10, 50'000, fcr::HeadroomClass::Firm);
  FCR_REQUIRE(reserved.has_value());

  fcr::RevokeRequest no_policy = fcr_test::revoke_request(fixture, "res-1", "attempt-2",
                                                          reserved->reservation.record.generation);
  no_policy.policy.reset();
  FCR_CHECK_ERROR(fixture.store.revoke(no_policy), fcr::ErrorCode::PolicyOverrideRequired);

  fcr::RevokeRequest no_actor = fcr_test::revoke_request(fixture, "res-1", "attempt-3",
                                                         reserved->reservation.record.generation);
  no_actor.actor = fcr::ActorRef();
  FCR_CHECK_ERROR(fixture.store.revoke(no_actor), fcr::ErrorCode::MissingField);

  const fcr::Result<fcr::RevokeOutcome> revoked = fixture.store.revoke(
      fcr_test::revoke_request(fixture, "res-1", "attempt-4", reserved->reservation.record.generation));
  FCR_REQUIRE(revoked.has_value());
  FCR_CHECK(revoked->reservation.record.state == fcr::ReservationState::Revoked);
  FCR_REQUIRE(revoked->reservation.record.termination.has_value());
  FCR_CHECK(revoked->reservation.record.termination->cause == fcr::TransitionCause::AuthorityRevocation);
  FCR_REQUIRE(revoked->reservation.record.termination->policy.has_value());
  FCR_CHECK_EQ(revoked->reservation.record.termination->policy->value(), std::string("policy-reclaim"));
  FCR_CHECK_EQ(revoked->reclaimed.size(), std::size_t{1});
}

FCR_TEST(ledger_lifecycle, a_guaranteed_commitment_needs_an_explicit_override_to_revoke) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = reserve_one(fixture, "res-1", "attempt-1", 10, 50'000);
  FCR_REQUIRE(reserved.has_value());

  FCR_CHECK_ERROR(fixture.store.revoke(fcr_test::revoke_request(fixture, "res-1", "attempt-2",
                                                                reserved->reservation.record.generation)),
                  fcr::ErrorCode::HeadroomNotRevocable);

  fcr::RevokeRequest forced = fcr_test::revoke_request(fixture, "res-1", "attempt-3",
                                                       reserved->reservation.record.generation);
  forced.allow_guaranteed_override = true;
  forced.cause = fcr::TransitionCause::FacilityOverride;
  const fcr::Result<fcr::RevokeOutcome> revoked = fixture.store.revoke(forced);
  FCR_REQUIRE(revoked.has_value());
  FCR_CHECK(revoked->reservation.record.termination->cause == fcr::TransitionCause::FacilityOverride);
}

FCR_TEST(ledger_lifecycle, an_opportunistic_commitment_revokes_with_a_policy_alone) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved =
      reserve_one(fixture, "res-1", "attempt-1", 10, 50'000, fcr::HeadroomClass::Opportunistic);
  FCR_REQUIRE(reserved.has_value());
  FCR_REQUIRE_OK(fixture.store.revoke(fcr_test::revoke_request(fixture, "res-1", "attempt-2",
                                                               reserved->reservation.record.generation)));
}

FCR_TEST(ledger_lifecycle, a_revocation_cannot_claim_a_claimant_transition_cause) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved =
      reserve_one(fixture, "res-1", "attempt-1", 10, 50'000, fcr::HeadroomClass::Firm);
  FCR_REQUIRE(reserved.has_value());
  fcr::RevokeRequest request = fcr_test::revoke_request(fixture, "res-1", "attempt-2",
                                                        reserved->reservation.record.generation);
  request.cause = fcr::TransitionCause::ClaimantRequest;
  FCR_CHECK_ERROR(fixture.store.revoke(request), fcr::ErrorCode::InvalidArgument);
  request.cause = fcr::TransitionCause::DeadlineElapsed;
  FCR_CHECK_ERROR(fixture.store.revoke(request), fcr::ErrorCode::InvalidArgument);
  request.cause = fcr::TransitionCause::Superseded;
  FCR_CHECK_ERROR(fixture.store.revoke(request), fcr::ErrorCode::InvalidArgument);
}

FCR_TEST(ledger_lifecycle, amendment_builds_a_contiguous_lineage) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = reserve_one(fixture, "res-1", "attempt-1", 5, 100'000);
  FCR_REQUIRE(reserved.has_value());

  fcr::ReservationGeneration generation = reserved->reservation.record.generation;
  for (int index = 0; index < 8; ++index) {
    const std::string attempt_name = "amend-" + std::to_string(index);
    fcr::AmendRequest request = fcr_test::amend_request(
        fixture, "res-1", attempt_name.c_str(), generation,
        {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", static_cast<std::uint64_t>(5 + index + 1))}, 1'100,
        100'000 + static_cast<std::uint64_t>(index));
    request.cause = fcr::AmendmentCause::CapacityIncrease;
    request.detail = "grow " + std::to_string(index);
    request.now = fcr::Tick(2'000 + static_cast<std::uint64_t>(index));
    const fcr::Result<fcr::AmendOutcome> amended = fixture.store.amend(request);
    FCR_REQUIRE(amended.has_value());
    FCR_CHECK_EQ(amended->previous_generation.value(), generation.value());
    generation = amended->reservation.record.generation;
  }
  FCR_CHECK_EQ(generation.value(), 9ULL);

  const fcr::Result<std::optional<fcr::ReservationView>> view = fixture.store.find(fcr_test::reservation_id("res-1"));
  FCR_REQUIRE(view.has_value());
  FCR_REQUIRE(view->has_value());
  FCR_REQUIRE((*view)->record.lineage.size() == 8);
  for (std::size_t index = 0; index < (*view)->record.lineage.size(); ++index) {
    const fcr::AmendmentEntry& entry = (*view)->record.lineage[index];
    FCR_CHECK_EQ(entry.generation.value(), index + 2);
    FCR_CHECK_EQ(entry.predecessor.value(), index + 1);
    FCR_CHECK_EQ(entry.at_tick.value(), 2'000 + index);
    FCR_CHECK(entry.cause == fcr::AmendmentCause::CapacityIncrease);
  }
  const fcr::Result<fcr::VerificationReport> report = fixture.store.verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
}

FCR_TEST(ledger_lifecycle, the_lineage_bound_is_enforced) {
  fcr::StoreOptions options;
  options.ledger.limits.max_lineage_entries = 3;
  fcr::Result<fcr::Store> opened = fcr::Store::in_memory(options);
  FCR_REQUIRE(opened.has_value());
  fcr::Store& store = opened.value();
  FCR_REQUIRE_OK(store.install_capacity(fcr_test::make_snapshot("snap", "site", 1, 100, kPools), fcr::Tick(1'000)));

  fcr_test::Fixture fixture;
  fixture.snapshot = fcr_test::make_snapshot("snap", "site", 1, 100, kPools);
  fixture.epoch = store.epoch().value();

  const fcr::Result<fcr::ReserveOutcome> reserved = store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 100'000));
  FCR_REQUIRE(reserved.has_value());
  fcr::ReservationGeneration generation = reserved->reservation.record.generation;
  for (int index = 0; index < 3; ++index) {
    const std::string attempt_name = "amend-" + std::to_string(index);
    const fcr::Result<fcr::AmendOutcome> amended = store.amend(fcr_test::amend_request(
        fixture, "res-1", attempt_name.c_str(), generation,
        {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", static_cast<std::uint64_t>(2 + index))}, 1'100,
        100'000));
    FCR_REQUIRE(amended.has_value());
    generation = amended->reservation.record.generation;
  }
  FCR_CHECK_ERROR(store.amend(fcr_test::amend_request(
                      fixture, "res-1", "amend-overflow", generation,
                      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 9)}, 1'100, 100'000)),
                  fcr::ErrorCode::LineageOverflow);
}

FCR_TEST(ledger_lifecycle, an_amendment_can_change_headroom_and_priority) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = reserve_one(fixture, "res-1", "attempt-1", 5, 100'000);
  FCR_REQUIRE(reserved.has_value());

  fcr::AmendRequest request = fcr_test::amend_request(
      fixture, "res-1", "attempt-2", reserved->reservation.record.generation,
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 5)}, 1'100, 100'000);
  request.headroom = fcr::HeadroomClass::Opportunistic;
  request.priority = fcr_test::priority("priority-low");
  request.cause = fcr::AmendmentCause::HeadroomChange;
  const fcr::Result<fcr::AmendOutcome> amended = fixture.store.amend(request);
  FCR_REQUIRE(amended.has_value());
  FCR_CHECK(amended->reservation.record.headroom == fcr::HeadroomClass::Opportunistic);
  FCR_CHECK_EQ(amended->reservation.record.priority.value(), std::string("priority-low"));

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ((*pools)[0].committed, 0ULL);
  FCR_CHECK_EQ((*pools)[0].protected_, 5ULL);
}

FCR_TEST(ledger_lifecycle, an_expiry_sweep_ends_only_what_is_due) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE(reserve_one(fixture, "res-soon", "attempt-1", 5, 2'000).has_value());
  FCR_REQUIRE(reserve_one(fixture, "res-later", "attempt-2", 5, 90'000).has_value());

  const fcr::Result<fcr::ExpireOutcome> first = fixture.store.expire(fcr_test::expire_request(fixture, "sweep-1", 1'999));
  FCR_REQUIRE(first.has_value());
  FCR_CHECK(first->expired.empty());

  const fcr::Result<fcr::ExpireOutcome> second = fixture.store.expire(fcr_test::expire_request(fixture, "sweep-2", 2'500));
  FCR_REQUIRE(second.has_value());
  FCR_CHECK_EQ(second->expired.size(), std::size_t{1});
  FCR_CHECK_EQ(second->expired[0].id.value(), std::string("res-soon"));

  const fcr::Result<fcr::ExpireOutcome> third = fixture.store.expire(fcr_test::expire_request(fixture, "sweep-3", 95'000));
  FCR_REQUIRE(third.has_value());
  FCR_CHECK_EQ(third->expired.size(), std::size_t{1});
  FCR_CHECK_EQ(third->expired[0].id.value(), std::string("res-later"));

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ((*pools)[0].committed, 0ULL);
  FCR_CHECK_EQ((*pools)[0].free, 50ULL);
}

FCR_TEST(ledger_lifecycle, provenance_survives_amendment_of_a_released_record_is_impossible) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = reserve_one(fixture, "res-1", "attempt-1", 5, 100'000);
  FCR_REQUIRE(reserved.has_value());
  FCR_REQUIRE_OK(fixture.store.release(fcr_test::release_request(fixture, "res-1", "attempt-2",
                                                                 reserved->reservation.record.generation)));
  // The terminal record keeps its creation tick and its lineage, and gains a
  // termination record; the generation does not advance.
  const fcr::Result<std::optional<fcr::ReservationView>> view = fixture.store.find(fcr_test::reservation_id("res-1"));
  FCR_REQUIRE(view.has_value());
  FCR_REQUIRE(view->has_value());
  FCR_CHECK_EQ((*view)->record.generation.value(), 1ULL);
  FCR_CHECK_EQ((*view)->record.created_at_tick.value(), 1'100ULL);
  FCR_CHECK_EQ((*view)->record.updated_at_tick.value(), 5'000ULL);
  FCR_CHECK((*view)->record.lineage.empty());
  FCR_CHECK((*view)->record.termination.has_value());
}

FCR_TEST(ledger_lifecycle, terminal_records_remain_visible_for_audit) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = reserve_one(fixture, "res-1", "attempt-1", 5, 100'000);
  FCR_REQUIRE(reserved.has_value());
  FCR_REQUIRE_OK(fixture.store.release(fcr_test::release_request(fixture, "res-1", "attempt-2",
                                                                 reserved->reservation.record.generation)));
  const fcr::Result<std::vector<fcr::ReservationView>> views = fixture.store.list();
  FCR_REQUIRE(views.has_value());
  FCR_CHECK_EQ(views->size(), std::size_t{1});
  FCR_CHECK((*views)[0].record.state == fcr::ReservationState::Released);
  FCR_CHECK_EQ((*views)[0].record.claims.size(), std::size_t{1});
  const fcr::Result<fcr::LedgerStatus> status = fixture.store.status();
  FCR_REQUIRE(status.has_value());
  FCR_CHECK_EQ(status->active_count, std::size_t{0});
  FCR_CHECK_EQ(status->reservation_count, std::size_t{1});
}

FCR_TEST(ledger_lifecycle, release_returns_exactly_the_claims_it_held) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1",
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 7),
       fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 70'000)},
      1'100, 100'000));
  FCR_REQUIRE(reserved.has_value());
  const fcr::Result<fcr::ReleaseOutcome> released = fixture.store.release(fcr_test::release_request(
      fixture, "res-1", "attempt-2", reserved->reservation.record.generation));
  FCR_REQUIRE(released.has_value());
  FCR_REQUIRE(released->released.size() == 2);
  FCR_CHECK(released->released[0].pool.kind == fcr::ResourceKind::Rack);
  FCR_CHECK_EQ(released->released[0].amount, 7ULL);
  FCR_CHECK(released->released[1].pool.kind == fcr::ResourceKind::Power);
  FCR_CHECK_EQ(released->released[1].amount, 70'000ULL);
}
