// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Ledger basics: capacity installation, reservation shape, claim canonicality,
// the acceptance path, and deterministic validation precedence.

#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

const std::vector<fcr_test::PoolSpec> kPools = {
    {fcr::ResourceKind::Rack, "hall-a", 40, 4, 6},
    {fcr::ResourceKind::Power, "feed-a", 2'000'000, 0, 100'000},
    {fcr::ResourceKind::Cooling, "loop-a", 1'800'000, 0, 0},
};

}  // namespace

FCR_TEST(ledger_basics, a_fresh_store_holds_no_capacity) {
  fcr::Result<fcr::Store> opened = fcr::Store::in_memory();
  FCR_REQUIRE(opened.has_value());
  fcr::Store& store = opened.value();
  FCR_CHECK_ERROR(store.capacity(), fcr::ErrorCode::NoCapacityInstalled);
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK(pools->empty());
  FCR_CHECK_ERROR(store.reserve(fcr_test::reserve_request(
                      fcr_test::Fixture{}, "res-1", "attempt-1",
                      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 10, 100)),
                  fcr::ErrorCode::AuthorityRequired);
}

FCR_TEST(ledger_basics, installing_capacity_publishes_a_revision) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::Revision> revision = fixture.store.revision();
  FCR_REQUIRE(revision.has_value());
  FCR_CHECK_EQ(revision->value(), 1ULL);

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_REQUIRE(pools->size() == 3);
  FCR_CHECK_EQ((*pools)[0].reservable, 30ULL);
  FCR_CHECK_EQ((*pools)[0].free, 30ULL);
  FCR_CHECK_EQ((*pools)[0].committed, 0ULL);
  FCR_CHECK_EQ((*pools)[0].protected_, 0ULL);
}

FCR_TEST(ledger_basics, capacity_cannot_be_installed_twice) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_CHECK_ERROR(fixture.store.install_capacity(fixture.snapshot, fcr::Tick(2'000)),
                  fcr::ErrorCode::CapacityAlreadyInstalled);
}

FCR_TEST(ledger_basics, installing_capacity_requires_a_tick) {
  fcr::Result<fcr::Store> opened = fcr::Store::in_memory();
  FCR_REQUIRE(opened.has_value());
  const fcr::CapacitySnapshot snapshot = fcr_test::make_snapshot("snap", "site", 1, 100, kPools);
  FCR_CHECK_ERROR(opened->install_capacity(snapshot, fcr::Tick(0)), fcr::ErrorCode::MissingField);
}

FCR_TEST(ledger_basics, a_reservation_binds_exactly_what_it_claims) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> outcome = fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1",
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 8),
       fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 320'000)},
      1'100, 50'000));
  FCR_REQUIRE(outcome.has_value());
  FCR_CHECK_EQ(outcome->reservation.record.generation.value(), 1ULL);
  FCR_CHECK_EQ(outcome->reservation.record.claims.size(), std::size_t{2});
  FCR_CHECK(outcome->reservation.active());
  FCR_CHECK(!outcome->reservation.record.termination.has_value());
  FCR_CHECK(outcome->reservation.record.lineage.empty());
  FCR_CHECK_EQ(outcome->reservation.record.source_generation.value(), 1ULL);
  FCR_CHECK_EQ(outcome->revision.value(), 2ULL);
  FCR_CHECK(!outcome->replayed);

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ((*pools)[0].committed, 8ULL);
  FCR_CHECK_EQ((*pools)[0].free, 22ULL);
  FCR_CHECK_EQ((*pools)[1].committed, 320'000ULL);
  FCR_CHECK_EQ((*pools)[2].committed, 0ULL);
  fcr_test::check_accounting_matches_reference(fixture.store, fixture.snapshot,
                                               fcr_test::reference_from_views(fixture.store.list().value()));
}

FCR_TEST(ledger_basics, opportunistic_headroom_is_accounted_as_protected) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::ReserveRequest request = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 5)}, 1'100, 50'000);
  request.headroom = fcr::HeadroomClass::Opportunistic;
  FCR_REQUIRE_OK(fixture.store.reserve(request));

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ((*pools)[0].committed, 0ULL);
  FCR_CHECK_EQ((*pools)[0].protected_, 5ULL);
  FCR_CHECK_EQ((*pools)[0].free, 25ULL);
}

FCR_TEST(ledger_basics, claims_are_stored_in_canonical_order) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> outcome = fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1",
      {fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 1'000),
       fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1),
       fcr_test::claim(fcr::ResourceKind::Cooling, "loop-a", 500)},
      1'100, 50'000));
  FCR_REQUIRE(outcome.has_value());
  for (std::size_t index = 1; index < outcome->reservation.record.claims.size(); ++index) {
    FCR_CHECK(outcome->reservation.record.claims[index - 1].pool <
              outcome->reservation.record.claims[index].pool);
  }
}

FCR_TEST(ledger_basics, an_empty_claim_set_is_refused) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_CHECK_ERROR(fixture.store.reserve(fcr_test::reserve_request(fixture, "res-1", "attempt-1", {}, 1'100, 50'000)),
                  fcr::ErrorCode::ClaimEmpty);
}

FCR_TEST(ledger_basics, a_zero_claim_is_refused) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_CHECK_ERROR(
      fixture.store.reserve(fcr_test::reserve_request(
          fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 0)}, 1'100, 50'000)),
      fcr::ErrorCode::ClaimAmountZero);
}

FCR_TEST(ledger_basics, a_duplicate_pool_in_one_claim_set_is_refused_not_merged) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_CHECK_ERROR(
      fixture.store.reserve(fcr_test::reserve_request(
          fixture, "res-1", "attempt-1",
          {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 3),
           fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 4)},
          1'100, 50'000)),
      fcr::ErrorCode::ClaimDuplicatePool);
}

FCR_TEST(ledger_basics, a_claim_on_an_unknown_pool_is_refused) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_CHECK_ERROR(
      fixture.store.reserve(fcr_test::reserve_request(
          fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-z", 1)}, 1'100, 50'000)),
      fcr::ErrorCode::PoolUnknown);
}

FCR_TEST(ledger_basics, a_claim_beyond_free_capacity_is_refused_with_the_pool_named) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> outcome = fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 31)}, 1'100, 50'000));
  FCR_REQUIRE(!outcome.has_value());
  FCR_CHECK(outcome.error().code() == fcr::ErrorCode::InsufficientCapacity);
  FCR_CHECK(outcome.error().subject().find("rack:hall-a") != std::string::npos);
  FCR_CHECK(outcome.error().subject().find("free=30") != std::string::npos);
}

FCR_TEST(ledger_basics, capacity_that_is_exactly_free_is_accepted) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 30)}, 1'100, 50'000)));
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ((*pools)[0].free, 0ULL);
}

FCR_TEST(ledger_basics, a_validity_interval_must_be_well_formed) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_CHECK_ERROR(
      fixture.store.reserve(fcr_test::reserve_request(
          fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 0, 50'000)),
      fcr::ErrorCode::InvalidInterval);
  FCR_CHECK_ERROR(
      fixture.store.reserve(fcr_test::reserve_request(
          fixture, "res-2", "attempt-2", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 500, 500)),
      fcr::ErrorCode::InvalidInterval);
  FCR_CHECK_ERROR(
      fixture.store.reserve(fcr_test::reserve_request(
          fixture, "res-3", "attempt-3", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 600, 500)),
      fcr::ErrorCode::InvalidInterval);
}

FCR_TEST(ledger_basics, an_already_elapsed_interval_is_refused) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::ReserveRequest request = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 100, 200);
  request.now = fcr::Tick(200);
  FCR_CHECK_ERROR(fixture.store.reserve(request), fcr::ErrorCode::DeadlineAlreadyPassed);
}

FCR_TEST(ledger_basics, a_duplicate_reservation_identity_is_refused) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 50'000)));
  FCR_CHECK_ERROR(
      fixture.store.reserve(fcr_test::reserve_request(
          fixture, "res-1", "attempt-2", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 50'000)),
      fcr::ErrorCode::ReservationAlreadyExists);
}

FCR_TEST(ledger_basics, required_references_must_be_present) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::ReserveRequest no_id = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 50'000);
  no_id.id = fcr::ReservationId();
  FCR_CHECK_ERROR(fixture.store.reserve(no_id), fcr::ErrorCode::MissingField);

  fcr::ReserveRequest no_attempt = no_id;
  no_attempt.id = fcr_test::reservation_id("res-1");
  no_attempt.attempt = fcr::AttemptId();
  FCR_CHECK_ERROR(fixture.store.reserve(no_attempt), fcr::ErrorCode::AttemptRequired);

  fcr::ReserveRequest no_claimant = no_attempt;
  no_claimant.attempt = fcr_test::attempt("attempt-1");
  no_claimant.claimant = fcr::ClaimantRef();
  FCR_CHECK_ERROR(fixture.store.reserve(no_claimant), fcr::ErrorCode::MissingField);

  fcr::ReserveRequest no_tenant = no_claimant;
  no_tenant.claimant = fcr_test::claimant("claimant-a");
  no_tenant.tenant = fcr::TenantRef();
  FCR_CHECK_ERROR(fixture.store.reserve(no_tenant), fcr::ErrorCode::MissingField);

  fcr::ReserveRequest no_service = no_tenant;
  no_service.tenant = fcr_test::tenant("tenant-a");
  no_service.service = fcr::ServiceRef();
  FCR_CHECK_ERROR(fixture.store.reserve(no_service), fcr::ErrorCode::MissingField);

  fcr::ReserveRequest no_priority = no_service;
  no_priority.service = fcr_test::service("service-a");
  no_priority.priority = fcr::PriorityRef();
  FCR_CHECK_ERROR(fixture.store.reserve(no_priority), fcr::ErrorCode::MissingField);

  fcr::ReserveRequest no_now = no_priority;
  no_now.priority = fcr_test::priority("priority-normal");
  no_now.now = fcr::Tick(0);
  FCR_CHECK_ERROR(fixture.store.reserve(no_now), fcr::ErrorCode::MissingField);
}

FCR_TEST(ledger_basics, the_source_generation_precondition_is_exact) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::ReserveRequest wrong = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 50'000);
  wrong.expected_source_generation = fcr::SourceGeneration(99);
  const fcr::Result<fcr::ReserveOutcome> outcome = fixture.store.reserve(wrong);
  FCR_REQUIRE(!outcome.has_value());
  FCR_CHECK(outcome.error().code() == fcr::ErrorCode::SourceGenerationStale);
  FCR_CHECK(outcome.error().subject().find("expected=99") != std::string::npos);

  fcr::ReserveRequest zero = wrong;
  zero.expected_source_generation = fcr::SourceGeneration(0);
  FCR_CHECK_ERROR(fixture.store.reserve(zero), fcr::ErrorCode::MissingField);
}

FCR_TEST(ledger_basics, a_stale_authority_epoch_is_refused_before_anything_else) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::ReserveRequest request = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 50'000);
  request.authority.epoch = fcr::AuthorityEpoch(fixture.epoch.value() + 7);
  FCR_CHECK_ERROR(fixture.store.reserve(request), fcr::ErrorCode::StaleAuthorityEpoch);

  request.authority.epoch = fixture.epoch;
  request.authority.revision = fcr::Revision(999);
  FCR_CHECK_ERROR(fixture.store.reserve(request), fcr::ErrorCode::StaleRevision);
}

FCR_TEST(ledger_basics, validation_precedence_is_fixed) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);

  // A request that is wrong in several ways at once reports the earliest check:
  // shape before attempt, attempt before authority, authority before capacity.
  fcr::ReserveRequest shape_and_authority = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 50'000);
  shape_and_authority.claims.clear();
  shape_and_authority.authority.epoch = fcr::AuthorityEpoch(999);
  FCR_CHECK_ERROR(fixture.store.reserve(shape_and_authority), fcr::ErrorCode::ClaimEmpty);

  fcr::ReserveRequest attempt_and_authority = shape_and_authority;
  attempt_and_authority.claims = {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)};
  attempt_and_authority.attempt = fcr::AttemptId();
  FCR_CHECK_ERROR(fixture.store.reserve(attempt_and_authority), fcr::ErrorCode::AttemptRequired);

  fcr::ReserveRequest authority_and_capacity = attempt_and_authority;
  authority_and_capacity.attempt = fcr_test::attempt("attempt-1");
  authority_and_capacity.claims = {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 999)};
  FCR_CHECK_ERROR(fixture.store.reserve(authority_and_capacity), fcr::ErrorCode::StaleAuthorityEpoch);

  fcr::ReserveRequest revision_and_capacity = authority_and_capacity;
  revision_and_capacity.authority.epoch = fixture.epoch;
  revision_and_capacity.authority.revision = fcr::Revision(999);
  FCR_CHECK_ERROR(fixture.store.reserve(revision_and_capacity), fcr::ErrorCode::StaleRevision);

  // Generation precedes state, which precedes capacity evidence.
  fcr::ReserveRequest capacity_and_deadline = revision_and_capacity;
  capacity_and_deadline.authority.revision = fcr::Revision(0);
  capacity_and_deadline.expected_source_generation = fcr::SourceGeneration(99);
  capacity_and_deadline.now = fcr::Tick(60'000);
  FCR_CHECK_ERROR(fixture.store.reserve(capacity_and_deadline), fcr::ErrorCode::SourceGenerationStale);
}

FCR_TEST(ledger_basics, a_closed_store_refuses_everything) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fixture.store.close();
  FCR_CHECK(fixture.store.closed());
  FCR_CHECK_ERROR(fixture.store.status(), fcr::ErrorCode::StoreClosed);
  FCR_CHECK_ERROR(fixture.store.list(), fcr::ErrorCode::StoreClosed);
  FCR_CHECK_ERROR(fixture.store.verify(), fcr::ErrorCode::StoreClosed);
  FCR_CHECK_ERROR(fixture.store.reserve(fcr_test::reserve_request(
                      fixture, "res-1", "attempt-1",
                      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 50'000)),
                  fcr::ErrorCode::StoreClosed);
  fixture.store.close();  // idempotent
  FCR_CHECK(fixture.store.closed());
}

FCR_TEST(ledger_basics, a_read_only_handle_refuses_every_mutation) {
  fcr_test::TempDir directory("readonly");
  const std::filesystem::path root = directory / "store";
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(fcr_test::make_snapshot("snap", "site", 1, 100, kPools),
                                             fcr::Tick(1'000)));
    created->close();
  }
  fcr::Result<fcr::Store> observer = fcr::Store::open_read_only(root);
  FCR_REQUIRE(observer.has_value());
  FCR_CHECK(observer->read_only());
  const fcr::Result<std::vector<fcr::PoolAccount>> observed = observer->pools();
  FCR_REQUIRE(observed.has_value());
  FCR_CHECK(observed->size() == 3);
  FCR_CHECK_ERROR(observer->install_capacity(fcr_test::make_snapshot("snap2", "site", 2, 200, kPools),
                                             fcr::Tick(2'000)),
                  fcr::ErrorCode::StoreReadOnly);
  FCR_CHECK_ERROR(observer->expire(fcr_test::expire_request(fcr_test::Fixture{}, "attempt-1", 5'000)),
                  fcr::ErrorCode::StoreReadOnly);
  observer->close();
}

FCR_TEST(ledger_basics, status_reports_the_authority_and_the_account) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 2)}, 1'100, 50'000)));
  const fcr::Result<fcr::LedgerStatus> status = fixture.store.status();
  FCR_REQUIRE(status.has_value());
  FCR_CHECK_EQ(status->revision.value(), 2ULL);
  FCR_CHECK_EQ(status->epoch.value(), fixture.epoch.value());
  FCR_CHECK_EQ(status->reservation_count, std::size_t{1});
  FCR_CHECK_EQ(status->active_count, std::size_t{1});
  FCR_CHECK_EQ(status->attempt_count, std::size_t{1});
  FCR_CHECK_EQ(status->pool_count, std::size_t{3});
  FCR_CHECK(status->capacity_fresh);
  FCR_CHECK(!status->durable);
  FCR_CHECK(!status->closed);
  FCR_CHECK_EQ(status->facility.value(), std::string("site-fixture"));
}

FCR_TEST(ledger_basics, a_volatile_store_reports_itself_as_not_durable) {
  fcr::Result<fcr::Store> opened = fcr::Store::in_memory();
  FCR_REQUIRE(opened.has_value());
  FCR_CHECK(!opened->durable());
  FCR_CHECK(opened->root().empty());
  const fcr::Result<fcr::VerificationReport> report = opened->verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
  FCR_CHECK_EQ(report->pool_count, std::size_t{0});
}

FCR_TEST(ledger_basics, a_durable_store_refuses_to_be_created_over_a_file) {
  fcr_test::TempDir directory("createfile");
  const std::filesystem::path path = directory / "not-a-directory";
  fcr_test::write_text_file(path, "hello");
  FCR_CHECK_ERROR(fcr::Store::create(path), fcr::ErrorCode::PathInvalid);
}

FCR_TEST(ledger_basics, a_store_cannot_be_created_in_a_non_empty_directory) {
  fcr_test::TempDir directory("notempty");
  fcr_test::write_text_file(directory / "keep-me.txt", "user data");
  FCR_CHECK_ERROR(fcr::Store::create(directory.path()), fcr::ErrorCode::StoreNotEmpty);
  // The unrelated file is untouched.
  FCR_CHECK_EQ(fcr_test::read_text_file(directory / "keep-me.txt"), std::string("user data"));
}

FCR_TEST(ledger_basics, a_volatile_mode_create_is_refused_with_a_pointer_to_in_memory) {
  fcr_test::TempDir directory("volatile");
  fcr::StoreOptions options;
  options.durability = fcr::DurabilityMode::Volatile;
  FCR_CHECK_ERROR(fcr::Store::create(directory / "store", options), fcr::ErrorCode::Unsupported);
}
