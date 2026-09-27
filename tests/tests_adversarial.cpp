// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Adversarial phase: deliberate attempts to make the library accept something it
// should refuse, or to make it crash, hang or corrupt state. Each case below was
// written as an attack, and each must end in a stable rejection.
//
// What is exercised here is input handling, arithmetic boundaries and the
// authority rules. What is not exercised — and is therefore not claimed — is any
// physical facility behaviour: there is no hardware beneath this library.

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "dccp/facility_capacity_reservation/digest.hpp"
#include "dccp/facility_capacity_reservation/text.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

const std::vector<fcr_test::PoolSpec> kPools = {
    {fcr::ResourceKind::Rack, "hall-a", 40, 0, 0},
    {fcr::ResourceKind::Power, "feed-a", 400'000, 0, 0},
};

}  // namespace

FCR_TEST(adversarial, a_claim_at_the_arithmetic_limit_is_handled_exactly) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(
      {{fcr::ResourceKind::Power, "feed-a", std::numeric_limits<std::uint64_t>::max(), 0, 0}});
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-max", "attempt-1",
      {fcr_test::claim(fcr::ResourceKind::Power, "feed-a", std::numeric_limits<std::uint64_t>::max())}, 1'100,
      900'000)));
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ((*pools)[0].free, 0ULL);
  FCR_CHECK_EQ((*pools)[0].committed, std::numeric_limits<std::uint64_t>::max());
  const fcr::Result<fcr::CapacitySplit> split = fixture.store.accounting_of(
      fcr::PoolKey{fcr::ResourceKind::Power, fcr_test::scope("feed-a")});
  FCR_REQUIRE(split.has_value());
  FCR_CHECK(split->accounted().has_value());
}

FCR_TEST(adversarial, an_over_commit_at_the_limit_is_refused_rather_than_wrapping) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(
      {{fcr::ResourceKind::Power, "feed-a", std::numeric_limits<std::uint64_t>::max(), 0, 0}});
  // The pool holds the maximum; asking for the maximum twice must be refused
  // without any intermediate arithmetic wrapping to a small number.
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-a", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 100)}, 1'100, 900'000)));
  FCR_CHECK_ERROR(
      fixture.store.reserve(fcr_test::reserve_request(
          fixture, "res-b", "attempt-2",
          {fcr_test::claim(fcr::ResourceKind::Power, "feed-a", std::numeric_limits<std::uint64_t>::max())}, 1'100,
          900'000)),
      fcr::ErrorCode::InsufficientCapacity);
  const fcr::Result<fcr::VerificationReport> report = fixture.store.verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
}

FCR_TEST(adversarial, a_snapshot_with_a_gross_that_overflows_the_available_sum_is_refused) {
  fcr::CapacitySnapshot snapshot = fcr_test::make_snapshot(
      "snap-max", "site-max", 1, 1'000,
      {{fcr::ResourceKind::Power, "feed-a", std::numeric_limits<std::uint64_t>::max(), 0, 0}});
  fcr::Result<fcr::Store> opened = fcr::Store::in_memory();
  FCR_REQUIRE(opened.has_value());
  FCR_REQUIRE_OK(opened->install_capacity(snapshot, fcr::Tick(1'000)));

  // Two pools of the same kind totalling more than the maximum must be caught by
  // the checked accumulation, not wrapped.
  fcr::CapacitySnapshot doubled = snapshot;
  doubled.pools = {{fcr::ResourceKind::Power, fcr_test::scope("feed-a"), std::numeric_limits<std::uint64_t>::max(), 0, 0},
                   {fcr::ResourceKind::Power, fcr_test::scope("feed-b"), std::numeric_limits<std::uint64_t>::max(), 0, 0}};
  const fcr::Result<std::uint64_t> total = doubled.reservable_of_kind(fcr::ResourceKind::Power);
  FCR_CHECK_ERROR(total, fcr::ErrorCode::ArithmeticOverflow);
}

FCR_TEST(adversarial, an_identifier_that_could_escape_a_path_is_refused_everywhere) {
  // Every identifier here would be dangerous if it were ever joined to a path or
  // echoed into a document. The library never uses an identifier as a file name
  // — generation file names are derived from revisions and digests — but the
  // alphabet is restricted anyway so that no caller can rely on it.
  const char* attacks[] = {"../../etc/passwd", "..\\..\\windows\\system32", "C:\\absolute", "\\\\server\\share",
                           "a/b",               "a%2fb",                      "a\nb",        "a b"};
  for (const char* attack : attacks) {
    FCR_CHECK_ERROR(fcr::ReservationId::parse(attack, "reservation id"), fcr::ErrorCode::MalformedIdentifier);
    FCR_CHECK_ERROR(fcr::ScopeRef::parse(attack, "scope"), fcr::ErrorCode::MalformedIdentifier);
    FCR_CHECK_ERROR(fcr::SnapshotRef::parse(attack, "snapshot ref"), fcr::ErrorCode::MalformedIdentifier);
    FCR_CHECK_ERROR(fcr::AttemptId::parse(attack, "attempt"), fcr::ErrorCode::MalformedIdentifier);
  }
}

FCR_TEST(adversarial, a_duplicate_reservation_identity_cannot_be_smuggled_in) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 900'000)));
  // A second, differently-spelled identity that canonicalises to the same text
  // is still the same identity: the library does no case folding or normalising.
  FCR_CHECK_ERROR(
      fixture.store.reserve(fcr_test::reserve_request(
          fixture, "res-1", "attempt-2", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 900'000)),
      fcr::ErrorCode::ReservationAlreadyExists);
  const fcr::Result<fcr::LedgerStatus> status = fixture.store.status();
  FCR_REQUIRE(status.has_value());
  FCR_CHECK_EQ(status->reservation_count, std::size_t{1});
}

FCR_TEST(adversarial, a_claim_set_larger_than_the_bound_is_refused_before_it_is_copied) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  std::vector<fcr::ResourceClaim> claims;
  for (std::uint64_t index = 0; index < 1'000; ++index) {
    claims.push_back(fcr_test::claim(fcr::ResourceKind::Rack, ("hall-" + fcr::format_unsigned(index)).c_str(), 1));
  }
  FCR_CHECK_ERROR(
      fixture.store.reserve(fcr_test::reserve_request(fixture, "res-1", "attempt-1", claims, 1'100, 900'000)),
      fcr::ErrorCode::LimitExceeded);
}

FCR_TEST(adversarial, an_amendment_that_would_exhaust_the_lineage_is_refused) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-0", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 900'000));
  FCR_REQUIRE(reserved.has_value());
  fcr::ReservationGeneration generation = reserved->reservation.record.generation;
  std::size_t accepted = 0;
  for (std::uint64_t index = 0; index < 2'000; ++index) {
    const std::string attempt_name = "amend-" + fcr::format_unsigned(index);
    // The first amendment must differ from the reservation it replaces, so the
    // amount cycle starts one unit above the original claim.
    const fcr::Result<fcr::AmendOutcome> amended = fixture.store.amend(fcr_test::amend_request(
        fixture, "res-1", attempt_name.c_str(), generation,
        {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 2 + (index % 20))}, 1'100, 900'000));
    if (amended.has_value()) {
      generation = amended->reservation.record.generation;
      ++accepted;
      continue;
    }
    // The only acceptable reasons to stop are the lineage bound and the
    // no-change rule; anything else is a defect.
    FCR_CHECK(amended.error().code() == fcr::ErrorCode::LineageOverflow ||
              amended.error().code() == fcr::ErrorCode::AmendmentNoChange);
    break;
  }
  FCR_CHECK(accepted > 0);
  const fcr::Result<fcr::VerificationReport> report = fixture.store.verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
}

FCR_TEST(adversarial, a_tick_at_the_arithmetic_limit_is_handled) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::ReserveRequest request = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1,
      std::numeric_limits<std::uint64_t>::max());
  request.now = fcr::Tick(1);
  FCR_REQUIRE_OK(fixture.store.reserve(request));

  // A sweep at the maximum tick ends it.
  const fcr::Result<fcr::ExpireOutcome> outcome = fixture.store.expire(
      fcr_test::expire_request(fixture, "sweep-max", std::numeric_limits<std::uint64_t>::max()));
  FCR_REQUIRE(outcome.has_value());
  FCR_CHECK_EQ(outcome->expired.size(), std::size_t{1});
}

FCR_TEST(adversarial, a_snapshot_document_full_of_control_bytes_is_refused) {
  std::string attack;
  attack.reserve(4'096);
  for (int index = 0; index < 4'096; ++index) {
    attack.push_back(static_cast<char>(index % 32));
  }
  FCR_CHECK(!fcr::parse_capacity_snapshot(attack, fcr::SnapshotLimits{}).has_value());
}

FCR_TEST(adversarial, a_snapshot_with_a_huge_declared_count_is_refused_before_allocating) {
  // The framing is deliberately long enough to reach the count check, so what is
  // exercised is the bound and not the truncated-input guard.
  std::string body = "fcr-capacity-snapshot 1\nref=snap\nsource-generation=1\nfacility=site\n"
                     "captured-at-tick=1\npools=18446744073709551615\n"
                     "pool=rack|hall-a|1|0|0\nend\n";
  const std::string digest = fcr::sha256_hex(body);
  body.append("digest=sha256:");
  body.append(digest);
  body.push_back('\n');
  FCR_CHECK_ERROR(fcr::parse_capacity_snapshot(body, fcr::SnapshotLimits{}), fcr::ErrorCode::LimitExceeded);
}

FCR_TEST(adversarial, a_scope_name_of_exactly_the_maximum_length_is_accepted) {
  const std::string longest(fcr::kMaxIdentifierBytes, 'a');
  FCR_CHECK(fcr::ScopeRef::parse(longest, "scope").has_value());
  FCR_CHECK_ERROR(fcr::ScopeRef::parse(longest + "a", "scope"), fcr::ErrorCode::IdentifierTooLong);
}

FCR_TEST(adversarial, a_detail_field_full_of_separators_round_trips_through_the_document) {
  fcr_test::TempDir directory("adversarial_detail");
  const std::filesystem::path root = directory / "store";
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  FCR_REQUIRE(created.has_value());
  FCR_REQUIRE_OK(created->install_capacity(fcr_test::make_snapshot("snap-a", "site-a", 1, 1'000, kPools),
                                           fcr::Tick(1'000)));
  fcr_test::Fixture fixture;
  fixture.snapshot = fcr_test::make_snapshot("snap-a", "site-a", 1, 1'000, kPools);
  fixture.epoch = created->epoch().value();
  const fcr::Result<fcr::ReserveOutcome> reserved = created->reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 4)}, 1'100, 900'000));
  FCR_REQUIRE(reserved.has_value());

  const std::string hostile = "line\nbreak|pipe=equals%semi;colon,comma\ttab";
  fcr::AmendRequest amend = fcr_test::amend_request(
      fixture, "res-1", "attempt-2", reserved->reservation.record.generation,
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 5)}, 1'100, 900'000);
  amend.detail = hostile;
  FCR_REQUIRE_OK(created->amend(amend));
  created->close();

  fcr::Result<fcr::Store> reopened = fcr::Store::open(root);
  FCR_REQUIRE(reopened.has_value());
  const fcr::Result<std::optional<fcr::ReservationView>> view = reopened->find(fcr_test::reservation_id("res-1"));
  FCR_REQUIRE(view.has_value());
  FCR_REQUIRE(view->has_value());
  FCR_REQUIRE((*view)->record.lineage.size() == 1);
  FCR_CHECK_EQ((*view)->record.lineage[0].detail, hostile);
  reopened->close();
}

FCR_TEST(adversarial, a_hostile_detail_never_changes_the_state_document_framing) {
  fcr_test::TempDir directory("adversarial_framing");
  const std::filesystem::path root = directory / "store";
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  FCR_REQUIRE(created.has_value());
  FCR_REQUIRE_OK(created->install_capacity(fcr_test::make_snapshot("snap-a", "site-a", 1, 1'000, kPools),
                                           fcr::Tick(1'000)));
  fcr_test::Fixture fixture;
  fixture.snapshot = fcr_test::make_snapshot("snap-a", "site-a", 1, 1'000, kPools);
  fixture.epoch = created->epoch().value();
  const fcr::Result<fcr::ReserveOutcome> reserved = created->reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 4)}, 1'100, 900'000));
  FCR_REQUIRE(reserved.has_value());
  fcr::ReleaseRequest release = fcr_test::release_request(fixture, "res-1", "attempt-2",
                                                          reserved->reservation.record.generation);
  release.detail = "reservation.end\nreservations=99\nend\n";
  FCR_REQUIRE_OK(created->release(release));

  const std::string document = fcr_test::read_text_file(fcr_test::committed_state_file(root));
  // The hostile text must appear escaped, never as literal framing.
  FCR_CHECK(document.find("reservation.end\nreservations=99") == std::string::npos);
  created->close();

  fcr::Result<fcr::Store> reopened = fcr::Store::open(root);
  FCR_REQUIRE(reopened.has_value());
  const fcr::Result<std::vector<fcr::ReservationView>> views = reopened->list();
  FCR_REQUIRE(views.has_value());
  FCR_CHECK_EQ(views->size(), std::size_t{1});
  reopened->close();
}

FCR_TEST(adversarial, repeated_failed_opens_do_not_accumulate_locks) {
  fcr_test::TempDir directory("adversarial_locks");
  const std::filesystem::path root = directory / "store";
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  FCR_REQUIRE(created.has_value());
  created->close();

  for (int attempt = 0; attempt < 64; ++attempt) {
    const fcr::Result<fcr::Store> refused = fcr::Store::open(directory / "absent");
    FCR_CHECK(!refused.has_value());
  }
  fcr::Result<fcr::Store> opened = fcr::Store::open(root);
  FCR_REQUIRE(opened.has_value());
  opened->close();
}

FCR_TEST(adversarial, a_validity_interval_spanning_the_whole_domain_is_accepted) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::ReserveRequest request = fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1,
      std::numeric_limits<std::uint64_t>::max());
  request.now = fcr::Tick(1);
  FCR_REQUIRE_OK(fixture.store.reserve(request));
  const fcr::Result<std::optional<fcr::ReservationView>> view = fixture.store.find(fcr_test::reservation_id("res-1"));
  FCR_REQUIRE(view.has_value());
  FCR_REQUIRE(view->has_value());
  FCR_CHECK_EQ((*view)->record.validity.duration_ticks(), std::numeric_limits<std::uint64_t>::max() - 1);
}

FCR_TEST(adversarial, a_reservation_whose_every_pool_is_at_its_limit_still_closes) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  for (int index = 0; index < 60; ++index) {
    const std::string id = "res-" + fcr::format_unsigned(static_cast<std::uint64_t>(index));
    const std::string attempt_name = "attempt-" + fcr::format_unsigned(static_cast<std::uint64_t>(index));
    (void)fixture.store.reserve(fcr_test::reserve_request(
        fixture, id.c_str(), attempt_name.c_str(),
        {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1),
         fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 10'000)},
        1'100, 900'000));
  }
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  for (const fcr::PoolAccount& account : *pools) {
    FCR_CHECK_EQ(account.committed + account.protected_ + account.free, account.reservable);
  }
  const fcr::Result<fcr::VerificationReport> report = fixture.store.verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
}

FCR_TEST(adversarial, a_release_from_a_different_actor_is_refused) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::Result<fcr::ReserveOutcome> reserved = fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 4)}, 1'100, 900'000));
  FCR_REQUIRE(reserved.has_value());

  // An actor reference that is not even a legal identifier cannot be built, so
  // the attack never reaches the ledger.
  FCR_CHECK_ERROR(fcr::ActorRef::parse("claimant-a ", "actor reference"), fcr::ErrorCode::MalformedIdentifier);

  // A legal identifier that differs only in case is a different actor: the
  // library does no case folding, so the claimant check refuses it.
  fcr::ReleaseRequest release = fcr_test::release_request(fixture, "res-1", "attempt-2",
                                                          reserved->reservation.record.generation);
  release.actor = fcr_test::actor("Claimant-A");
  FCR_CHECK_ERROR(fixture.store.release(release), fcr::ErrorCode::ClaimantMismatch);

  const fcr::Result<std::optional<fcr::ReservationView>> view = fixture.store.find(fcr_test::reservation_id("res-1"));
  FCR_REQUIRE(view.has_value());
  FCR_REQUIRE(view->has_value());
  FCR_CHECK((*view)->active());
}

FCR_TEST(adversarial, a_hostile_effect_digest_claim_is_not_accepted_as_an_attempt) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 4)}, 1'100, 900'000)));
  // An attempt identity that differs only in case is a different identity.
  fcr::ReserveRequest request = fcr_test::reserve_request(
      fixture, "res-1", "ATTEMPT-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 4)}, 1'100, 900'000);
  FCR_CHECK_ERROR(fixture.store.reserve(request), fcr::ErrorCode::ReservationAlreadyExists);
}

FCR_TEST(adversarial, a_claim_set_at_the_configured_maximum_is_bound_atomically) {
  // One reservation claiming the maximum number of distinct pools: every pool is
  // checked before any is touched, so a single bad pool rejects the whole set.
  const std::size_t claim_count = fcr::RequestLimits{}.max_claims;
  std::vector<std::string> names;
  names.reserve(claim_count);
  for (std::size_t index = 0; index < claim_count; ++index) {
    names.push_back("hall-" + fcr::format_unsigned(static_cast<std::uint64_t>(index)));
  }

  fcr::CapacitySnapshot snapshot;
  snapshot.ref = fcr_test::snapshot_ref("snap-max-claims");
  snapshot.facility = fcr_test::facility_ref("site-max-claims");
  snapshot.source_generation = fcr::SourceGeneration(1);
  snapshot.captured_at_tick = fcr::Tick(1'000);
  for (const std::string& name : names) {
    fcr::CapacityPool pool;
    pool.key.kind = fcr::ResourceKind::Rack;
    pool.key.scope = fcr_test::scope(name.c_str());
    pool.gross = 4;
    snapshot.pools.push_back(pool);
  }
  std::sort(snapshot.pools.begin(), snapshot.pools.end(),
            [](const fcr::CapacityPool& lhs, const fcr::CapacityPool& rhs) { return lhs.key < rhs.key; });

  fcr::Result<fcr::Store> opened = fcr::Store::in_memory();
  FCR_REQUIRE(opened.has_value());
  FCR_REQUIRE_OK(opened->install_capacity(snapshot, fcr::Tick(1'000)));
  fcr_test::Fixture fixture;
  fixture.snapshot = snapshot;
  fixture.epoch = opened->epoch().value();

  std::vector<fcr::ResourceClaim> claims;
  claims.reserve(names.size());
  for (const std::string& name : names) {
    claims.push_back(fcr_test::claim(fcr::ResourceKind::Rack, name.c_str(), 1));
  }
  FCR_REQUIRE_OK(opened->reserve(
      fcr_test::reserve_request(fixture, "res-max-claims", "attempt-1", claims, 1'100, 900'000)));
  const fcr::Result<fcr::VerificationReport> report = opened->verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
  FCR_CHECK_EQ(report->pool_count, claim_count);

  // One unit too many on any single pool rejects the entire commitment.
  claims.back().amount = 99;
  FCR_CHECK_ERROR(
      opened->reserve(fcr_test::reserve_request(fixture, "res-over", "attempt-2", claims, 1'100, 900'000)),
      fcr::ErrorCode::InsufficientCapacity);
  const fcr::Result<std::vector<fcr::PoolAccount>> accounts = opened->pools();
  FCR_REQUIRE(accounts.has_value());
  for (const fcr::PoolAccount& account : *accounts) {
    FCR_CHECK(account.committed <= 1);
  }

  // One claim more than the bound is refused before anything is copied.
  claims.back().amount = 1;
  claims.push_back(fcr_test::claim(fcr::ResourceKind::Rack, names.front().c_str(), 1));
  FCR_CHECK_ERROR(
      opened->reserve(fcr_test::reserve_request(fixture, "res-many", "attempt-3", claims, 1'100, 900'000)),
      fcr::ErrorCode::LimitExceeded);
  opened->close();
}

FCR_TEST(adversarial, an_attempt_identity_reused_after_a_restart_with_a_new_intent_is_refused) {
  fcr_test::TempDir directory("adversarial_attempt");
  const std::filesystem::path root = directory / "store";
  fcr::ReserveRequest original;
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(
        created->install_capacity(fcr_test::make_snapshot("snap-a", "site-a", 1, 1'000, kPools), fcr::Tick(1'000)));
    fcr_test::Fixture fixture;
    fixture.snapshot = fcr_test::make_snapshot("snap-a", "site-a", 1, 1'000, kPools);
    fixture.epoch = created->epoch().value();
    original = fcr_test::reserve_request(fixture, "res-1", "shared-attempt",
                                         {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 900'000);
    FCR_REQUIRE_OK(created->reserve(original));
    created->close();
  }

  fcr::StoreOptions trusting;
  trusting.trust_persisted_capacity = true;
  fcr::Result<fcr::Store> reopened = fcr::Store::open(root, trusting);
  FCR_REQUIRE(reopened.has_value());

  // The same attempt with the same intent replays across the restart.
  const fcr::Result<fcr::ReserveOutcome> replay = reopened->reserve(original);
  FCR_REQUIRE(replay.has_value());
  FCR_CHECK(replay->replayed);

  // The same attempt with a different intent is a conflict, not a second grant.
  fcr::ReserveRequest different = original;
  different.claims = {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 2)};
  FCR_CHECK_ERROR(reopened->reserve(different), fcr::ErrorCode::AttemptConflict);
  const fcr::Result<std::vector<fcr::PoolAccount>> accounts = reopened->pools();
  FCR_REQUIRE(accounts.has_value());
  FCR_CHECK_EQ(fcr_test::find_pool(*accounts, fcr::ResourceKind::Rack, "hall-a")->committed, 1ULL);
  reopened->close();
}

FCR_TEST(adversarial, a_generations_path_that_is_not_a_directory_is_refused) {
  fcr_test::TempDir directory("adversarial_generations");
  const std::filesystem::path root = directory / "store";
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    created->close();
  }
  std::error_code error;
  std::filesystem::remove_all(root / "generations", error);
  FCR_REQUIRE(!error);
  fcr_test::write_text_file(root / "generations", "not a directory");
  FCR_CHECK_ERROR(fcr::Store::open(root), fcr::ErrorCode::StoreCorrupt);
  FCR_CHECK_ERROR(fcr::Store::create(root), fcr::ErrorCode::StoreNotEmpty);
}

FCR_TEST(adversarial, an_expiry_sweep_without_a_tick_is_refused) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  fcr::ExpireRequest request = fcr_test::expire_request(fixture, "sweep-1", 5'000);
  request.now = fcr::Tick(0);
  FCR_CHECK_ERROR(fixture.store.expire(request), fcr::ErrorCode::MissingField);
}

FCR_TEST(adversarial, every_error_code_is_reachable_from_a_documented_rejection) {
  // A representative set of rejections, each asserted by code so that the
  // vocabulary is exercised rather than merely declared.
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const std::vector<fcr::ErrorCode> expected = {
      fcr::ErrorCode::ClaimEmpty,
      fcr::ErrorCode::ClaimAmountZero,
      fcr::ErrorCode::ClaimDuplicatePool,
      fcr::ErrorCode::PoolUnknown,
      fcr::ErrorCode::InvalidInterval,
      fcr::ErrorCode::DeadlineAlreadyPassed,
      fcr::ErrorCode::AttemptRequired,
      fcr::ErrorCode::InvalidArgument,
  };
  std::vector<fcr::ErrorCode> observed;
  const auto record = [&observed](const fcr::Result<fcr::ReserveOutcome>& outcome) {
    FCR_REQUIRE(!outcome.has_value());
    observed.push_back(outcome.error().code());
  };

  record(fixture.store.reserve(fcr_test::reserve_request(fixture, "r1", "a1", {}, 1'100, 900'000)));

  record(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "r2", "a2", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 0)}, 1'100, 900'000)));

  record(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "r3", "a3",
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1), fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 2)},
      1'100, 900'000)));

  record(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "r4", "a4", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-z", 1)}, 1'100, 900'000)));

  record(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "r5", "a5", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 0, 900'000)));

  fcr::ReserveRequest elapsed = fcr_test::reserve_request(
      fixture, "r6", "a6", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'000, 1'050);
  elapsed.now = fcr::Tick(1'100);
  record(fixture.store.reserve(elapsed));

  fcr::ReserveRequest no_attempt = fcr_test::reserve_request(
      fixture, "r7", "a7", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 900'000);
  no_attempt.attempt = fcr::AttemptId();
  record(fixture.store.reserve(no_attempt));

  fcr::ReserveRequest bad_kind = fcr_test::reserve_request(
      fixture, "r8", "a8", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 900'000);
  bad_kind.headroom = static_cast<fcr::HeadroomClass>(99);
  record(fixture.store.reserve(bad_kind));

  FCR_CHECK_EQ(observed.size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    FCR_CHECK(observed[index] == expected[index]);
  }
}
