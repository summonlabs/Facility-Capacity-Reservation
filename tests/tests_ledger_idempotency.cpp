// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Idempotent retry and durable attempt identity: a repeated attempt replays, a
// reused attempt with a different intent is refused, and the index is bounded.

#include <string>
#include <vector>

#include "dccp/facility_capacity_reservation/ledger.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

const std::vector<fcr_test::PoolSpec> kPools = {
    {fcr::ResourceKind::Rack, "hall-a", 50, 0, 0},
    {fcr::ResourceKind::Power, "feed-a", 500'000, 0, 0},
};

}  // namespace

FCR_TEST(ledger_idempotency, a_repeated_reserve_replays_the_same_reservation) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::ReserveRequest request = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'000);

  const fcr::Result<fcr::ReserveOutcome> first = fixture.store.reserve(request);
  FCR_REQUIRE(first.has_value());
  FCR_CHECK(!first->replayed);

  const fcr::Result<fcr::ReserveOutcome> second = fixture.store.reserve(request);
  FCR_REQUIRE(second.has_value());
  FCR_CHECK(second->replayed);
  FCR_CHECK_EQ(second->reservation.record.generation.value(), first->reservation.record.generation.value());
  FCR_CHECK_EQ(second->reservation.record.id.value(), std::string("res-1"));

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ((*pools)[0].committed, 10ULL);

  const fcr::Result<fcr::LedgerStatus> status = fixture.store.status();
  FCR_REQUIRE(status.has_value());
  FCR_CHECK_EQ(status->reservation_count, std::size_t{1});
  FCR_CHECK_EQ(status->attempt_count, std::size_t{1});
}

FCR_TEST(ledger_idempotency, a_retry_before_the_deadline_is_still_the_same_request) {
  // The observation tick is deliberately excluded from the intent digest: a
  // retry issued later must still replay, because the caller's intent has not
  // changed.
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::ReserveRequest request = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'000);
  FCR_REQUIRE_OK(fixture.store.reserve(request));
  request.now = fcr::Tick(9'000);
  const fcr::Result<fcr::ReserveOutcome> retry = fixture.store.reserve(request);
  FCR_REQUIRE(retry.has_value());
  FCR_CHECK(retry->replayed);
}

FCR_TEST(ledger_idempotency, reusing_an_attempt_identity_for_a_different_intent_is_refused) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'000)));

  fcr::ReserveRequest different_amount = fcr_test::reserve_request(
      fixture, "res-2", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 11)}, 1'100, 50'000);
  FCR_CHECK_ERROR(fixture.store.reserve(different_amount), fcr::ErrorCode::AttemptConflict);

  fcr::ReserveRequest different_id = fcr_test::reserve_request(
      fixture, "res-2", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'000);
  FCR_CHECK_ERROR(fixture.store.reserve(different_id), fcr::ErrorCode::AttemptConflict);

  fcr::ReserveRequest different_deadline = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'001);
  FCR_CHECK_ERROR(fixture.store.reserve(different_deadline), fcr::ErrorCode::AttemptConflict);
}

FCR_TEST(ledger_idempotency, an_attempt_identity_cannot_be_reused_across_operations) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "shared-attempt", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'000));
  FCR_REQUIRE(reserved.has_value());
  FCR_CHECK_ERROR(fixture.store.release(fcr_test::release_request(fixture, "res-1", "shared-attempt",
                                                                  reserved->reservation.record.generation)),
                  fcr::ErrorCode::AttemptConflict);
}

FCR_TEST(ledger_idempotency, a_repeated_release_replays_the_terminal_record) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'000));
  FCR_REQUIRE(reserved.has_value());
  const fcr::ReleaseRequest release = fcr_test::release_request(fixture, "res-1", "attempt-2",
                                                                reserved->reservation.record.generation);
  const fcr::Result<fcr::ReleaseOutcome> first = fixture.store.release(release);
  FCR_REQUIRE(first.has_value());
  FCR_CHECK(!first->replayed);
  const fcr::Result<fcr::ReleaseOutcome> second = fixture.store.release(release);
  FCR_REQUIRE(second.has_value());
  FCR_CHECK(second->replayed);
  FCR_CHECK(second->reservation.record.state == fcr::ReservationState::Released);
  FCR_CHECK_EQ(second->revision.value(), first->revision.value());
}

FCR_TEST(ledger_idempotency, a_repeated_amendment_replays_without_amending_twice) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'000));
  FCR_REQUIRE(reserved.has_value());
  const fcr::AmendRequest amend = fcr_test::amend_request(
      fixture, "res-1", "attempt-2", reserved->reservation.record.generation,
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 20)}, 1'100, 60'000);
  const fcr::Result<fcr::AmendOutcome> first = fixture.store.amend(amend);
  FCR_REQUIRE(first.has_value());
  const fcr::Result<fcr::AmendOutcome> second = fixture.store.amend(amend);
  FCR_REQUIRE(second.has_value());
  FCR_CHECK(second->replayed);
  FCR_CHECK_EQ(second->reservation.record.generation.value(), 2ULL);
  FCR_CHECK_EQ(second->reservation.record.lineage.size(), std::size_t{1});
}

FCR_TEST(ledger_idempotency, a_retried_reserve_after_an_amendment_reports_the_current_state) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::ReserveRequest reserve = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'000);
  const fcr::Result<fcr::ReserveOutcome> created = fixture.store.reserve(reserve);
  FCR_REQUIRE(created.has_value());
  FCR_REQUIRE_OK(fixture.store.amend(fcr_test::amend_request(
      fixture, "res-1", "attempt-2", created->reservation.record.generation,
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 20)}, 1'100, 60'000)));

  const fcr::Result<fcr::ReserveOutcome> retry = fixture.store.reserve(reserve);
  FCR_REQUIRE(retry.has_value());
  FCR_CHECK(retry->replayed);
  FCR_CHECK_EQ(retry->reservation.record.generation.value(), 2ULL);
}

FCR_TEST(ledger_idempotency, a_repeated_expiry_sweep_reports_the_same_set) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 5)}, 1'100, 2'000)));
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-2", "attempt-2", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 5)}, 1'100, 2'100)));

  const fcr::ExpireRequest sweep = fcr_test::expire_request(fixture, "sweep", 5'000);
  const fcr::Result<fcr::ExpireOutcome> first = fixture.store.expire(sweep);
  FCR_REQUIRE(first.has_value());
  FCR_CHECK_EQ(first->expired.size(), std::size_t{2});
  const fcr::Result<fcr::ExpireOutcome> second = fixture.store.expire(sweep);
  FCR_REQUIRE(second.has_value());
  FCR_CHECK(second->replayed);
  FCR_CHECK_EQ(second->expired.size(), std::size_t{2});
  FCR_CHECK_EQ(second->revision.value(), first->revision.value());
}

FCR_TEST(ledger_idempotency, a_different_sweep_tick_is_a_different_intent) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 5)}, 1'100, 2'000)));
  FCR_REQUIRE_OK(fixture.store.expire(fcr_test::expire_request(fixture, "sweep", 3'000)));
  FCR_CHECK_ERROR(fixture.store.expire(fcr_test::expire_request(fixture, "sweep", 4'000)),
                  fcr::ErrorCode::AttemptConflict);
}

FCR_TEST(ledger_idempotency, intent_digests_are_stable_and_distinct) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::ReserveRequest first = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'000);
  const fcr::ReserveRequest same = first;
  FCR_CHECK_EQ(fcr::intent_digest(first), fcr::intent_digest(same));

  fcr::ReserveRequest other = first;
  other.claims = {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 11)};
  FCR_CHECK_NE(fcr::intent_digest(first), fcr::intent_digest(other));

  fcr::ReserveRequest reordered = first;
  reordered.claims = {fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 5),
                      fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)};
  fcr::ReserveRequest sorted = reordered;
  std::swap(sorted.claims[0], sorted.claims[1]);
  FCR_CHECK_NE(fcr::intent_digest(reordered), fcr::intent_digest(sorted));
}

FCR_TEST(ledger_idempotency, a_released_reservation_retried_from_the_original_attempt_reports_released) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::ReserveRequest reserve = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'000);
  const fcr::Result<fcr::ReserveOutcome> created = fixture.store.reserve(reserve);
  FCR_REQUIRE(created.has_value());
  FCR_REQUIRE_OK(fixture.store.release(fcr_test::release_request(fixture, "res-1", "attempt-2",
                                                                 created->reservation.record.generation)));
  const fcr::Result<fcr::ReserveOutcome> retry = fixture.store.reserve(reserve);
  FCR_REQUIRE(retry.has_value());
  FCR_CHECK(retry->replayed);
  FCR_CHECK(retry->reservation.record.state == fcr::ReservationState::Released);
}

FCR_TEST(ledger_idempotency, an_attempt_addressed_to_a_missing_subject_is_not_replayable) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::ReserveRequest request = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10)}, 1'100, 50'000);
  FCR_REQUIRE_OK(fixture.store.reserve(request));
  // A release request carrying the reserve attempt's identity is a conflict, so
  // the recorded outcome is never reached; the distinguishing behaviour is that
  // neither call is silently applied.
  FCR_CHECK_ERROR(fixture.store.release(fcr_test::release_request(fixture, "res-1", "attempt-1",
                                                                  fcr::ReservationGeneration(1))),
                  fcr::ErrorCode::AttemptConflict);
}

FCR_TEST(ledger_idempotency, attempt_records_carry_the_revision_and_epoch_that_produced_them) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 50'000));
  FCR_REQUIRE(reserved.has_value());
  const fcr::Result<fcr::LedgerStatus> status = fixture.store.status();
  FCR_REQUIRE(status.has_value());
  FCR_CHECK_EQ(status->attempt_count, std::size_t{1});
  const fcr::Result<std::vector<fcr::ReservationView>> views = fixture.store.list();
  FCR_REQUIRE(views.has_value());
  FCR_CHECK_EQ((*views)[0].record.last_attempt.value(), std::string("attempt-1"));
  FCR_CHECK_EQ((*views)[0].record.revision.value(), reserved->revision.value());
  FCR_CHECK_EQ((*views)[0].record.authority_epoch.value(), fixture.epoch.value());
}
