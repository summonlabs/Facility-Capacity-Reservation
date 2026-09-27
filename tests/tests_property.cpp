// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Property and randomized tests. Every case is driven by the seed the framework
// prints, so a failure is reproducible with --seed=<value>.
//
// The central property is the accounting closure: at every published revision,
// for every pool, committed + protected + free equals the reservable capacity
// derived from the consumed snapshot. It is checked against the independent
// reference model, never against the library's own arithmetic.

#include <algorithm>
#include <string>
#include <vector>

#include "dccp/facility_capacity_reservation/digest.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

const std::vector<fcr_test::PoolSpec> kPools = {
    {fcr::ResourceKind::Rack, "hall-a", 60, 6, 4},
    {fcr::ResourceKind::Power, "feed-a", 600'000, 10'000, 20'000},
    {fcr::ResourceKind::Cooling, "loop-a", 500'000, 0, 0},
    {fcr::ResourceKind::Space, "hall-a", 600, 0, 50},
    {fcr::ResourceKind::FacilityService, "lifts", 8, 1, 1},
};

constexpr fcr::ResourceKind kKinds[] = {fcr::ResourceKind::Rack, fcr::ResourceKind::Power,
                                        fcr::ResourceKind::Cooling, fcr::ResourceKind::Space,
                                        fcr::ResourceKind::FacilityService};

const char* scope_of(fcr::ResourceKind kind) {
  switch (kind) {
    case fcr::ResourceKind::Power:
      return "feed-a";
    case fcr::ResourceKind::Cooling:
      return "loop-a";
    case fcr::ResourceKind::FacilityService:
      return "lifts";
    case fcr::ResourceKind::Space:
    case fcr::ResourceKind::Rack:
      return "hall-a";
  }
  return "hall-a";
}

fcr::HeadroomClass headroom_of(std::uint64_t value) {
  switch (value % 3) {
    case 0:
      return fcr::HeadroomClass::Guaranteed;
    case 1:
      return fcr::HeadroomClass::Firm;
    default:
      return fcr::HeadroomClass::Opportunistic;
  }
}

std::string name_of(const char* prefix, std::uint64_t index) {
  return std::string(prefix) + "-" + fcr::format_unsigned(index);
}

/// Runs one randomized workload and asserts the closure after every step.
void run_workload(std::uint64_t seed, std::size_t steps) {
  fcr_test::Rng rng(seed);
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  const fcr::CapacitySnapshot initial = fixture.snapshot;
  fcr::CapacitySnapshot current = initial;
  std::vector<std::string> live;

  for (std::size_t step = 0; step < steps; ++step) {
    fcr_test::set_case_context("seed=" + fcr::format_unsigned(seed) + " step=" +
                               fcr::format_unsigned(static_cast<std::uint64_t>(step)));
    const std::uint64_t choice = rng.below(100);

    if (choice < 45 || live.empty()) {
      const std::string id = name_of("res", rng.below(1'000'000));
      const std::string attempt_name = name_of("a", rng.below(1'000'000));
      std::vector<fcr::ResourceClaim> claims;
      const std::uint64_t claim_count = 1 + rng.below(3);
      for (std::uint64_t index = 0; index < claim_count; ++index) {
        const fcr::ResourceKind kind = kKinds[rng.below(5)];
        const std::uint64_t amount = 1 + rng.below(30);
        bool duplicate = false;
        for (const fcr::ResourceClaim& existing : claims) {
          if (existing.pool.kind == kind) {
            duplicate = true;
          }
        }
        if (duplicate) {
          continue;
        }
        claims.push_back(fcr_test::claim(kind, scope_of(kind), amount));
      }
      fcr::ReserveRequest request = fcr_test::reserve_request(
          fixture, id.c_str(), attempt_name.c_str(), claims, 1'100,
          2'000 + rng.below(900'000));
      request.headroom = headroom_of(rng.next());
      const fcr::Result<fcr::ReserveOutcome> outcome = fixture.store.reserve(request);
      if (outcome.has_value()) {
        live.push_back(id);
      }
      continue;
    }

    const std::size_t index = static_cast<std::size_t>(rng.below(live.size()));
    const std::string id = live[index];
    const fcr::Result<std::optional<fcr::ReservationView>> view = fixture.store.find(fcr_test::reservation_id(id.c_str()));
    if (!view.has_value() || !view->has_value() || !(*view)->active()) {
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
      continue;
    }
    const fcr::ReservationGeneration generation = (*view)->record.generation;

    if (choice < 65) {
      const fcr::ResourceKind kind = kKinds[rng.below(5)];
      fcr::AmendRequest request = fcr_test::amend_request(
          fixture, id.c_str(), name_of("amend", rng.next()).c_str(), generation,
          {fcr_test::claim(kind, scope_of(kind), 1 + rng.below(20))}, 1'100, 2'000 + rng.below(900'000));
      request.cause = fcr::AmendmentCause::CapacityIncrease;
      (void)fixture.store.amend(request);
    } else if (choice < 90) {
      (void)fixture.store.release(
          fcr_test::release_request(fixture, id.c_str(), name_of("rel", rng.next()).c_str(), generation));
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
    } else {
      (void)fixture.store.expire(fcr_test::expire_request(fixture, name_of("sweep", rng.next()).c_str(),
                                                          2'000 + rng.below(900'000)));
    }

    const fcr::Result<fcr::VerificationReport> report = fixture.store.verify();
    FCR_REQUIRE(report.has_value());
    FCR_CHECK(report->ok);

    const fcr::Result<std::vector<fcr::ReservationView>> views = fixture.store.list();
    FCR_REQUIRE(views.has_value());
    fcr_test::check_accounting_matches_reference(fixture.store, current,
                                                 fcr_test::reference_from_views(*views));
  }
}

}  // namespace

FCR_TEST(property, closure_holds_under_a_randomized_workload) {
  const std::uint64_t base = fcr_test::current_seed();
  for (std::uint64_t variant = 0; variant < 6; ++variant) {
    run_workload(base + variant * 7919ULL, 120);
  }
}

FCR_TEST(property, closure_holds_under_a_randomized_workload_with_reconciliation) {
  const std::uint64_t base = fcr_test::current_seed();
  for (std::uint64_t variant = 0; variant < 4; ++variant) {
    fcr_test::Rng rng(base + variant * 104'729ULL);
    fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
    fcr::CapacitySnapshot current = fixture.snapshot;
    std::vector<std::string> live;

    for (std::size_t step = 0; step < 40; ++step) {
      fcr_test::set_case_context("variant=" + fcr::format_unsigned(variant) + " step=" +
                                 fcr::format_unsigned(static_cast<std::uint64_t>(step)));
      const std::uint64_t choice = rng.below(100);
      if (choice < 55 || live.empty()) {
        const std::string id = name_of("res", rng.below(1'000'000));
        const std::string attempt_name = name_of("a", rng.below(1'000'000));
        fcr::ReserveRequest request = fcr_test::reserve_request(
            fixture, id.c_str(), attempt_name.c_str(),
            {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1 + rng.below(20))}, 1'100,
            2'000 + rng.below(900'000));
        request.headroom = headroom_of(rng.next());
        if (fixture.store.reserve(request).has_value()) {
          live.push_back(id);
        }
      } else {
        // Capacity is only ever grown here, so the reconciliation never has to
        // fence anything: the fencing path has its own deterministic tests, and
        // this property is about the closure surviving adoption.
        const std::uint64_t extra = rng.below(20);
        const fcr::PoolKey rack_pool{fcr::ResourceKind::Rack, fcr_test::scope("hall-a")};
        const fcr::CapacityPool* previous_rack = current.find(rack_pool);
        const std::uint64_t previous_gross = previous_rack == nullptr ? 0 : previous_rack->gross;
        const std::uint64_t gross = std::max(previous_gross, 60ULL + extra);
        current = fcr_test::make_snapshot(
            "snap-next", "site-fixture", current.source_generation.value() + 1, 9'000,
            {{fcr::ResourceKind::Rack, "hall-a", gross, 6, 4},
             {fcr::ResourceKind::Power, "feed-a", 600'000, 10'000, 20'000},
             {fcr::ResourceKind::Cooling, "loop-a", 500'000, 0, 0},
             {fcr::ResourceKind::Space, "hall-a", 600, 0, 50},
             {fcr::ResourceKind::FacilityService, "lifts", 8, 1, 1}});
        fcr::ReconcileRequest request = fcr_test::reconcile_request(
            fixture, name_of("reconcile", rng.next()).c_str(), current,
            fixture.store.status().value().source_generation, fcr::ReconcileMode::Enforce);
        const fcr::Result<fcr::ReconcileOutcome> outcome = fixture.store.reconcile(request);
        FCR_REQUIRE(outcome.has_value());
        FCR_CHECK(outcome->adopted);
        FCR_CHECK(outcome->fenced.empty());
        // The ledger now holds the new snapshot, so the reference model must be
        // compared against it.
        fixture.snapshot = current;
      }

      const fcr::Result<fcr::VerificationReport> report = fixture.store.verify();
      FCR_REQUIRE(report.has_value());
      FCR_CHECK(report->ok);
      const fcr::Result<std::vector<fcr::ReservationView>> views = fixture.store.list();
      FCR_REQUIRE(views.has_value());
      fcr_test::check_accounting_matches_reference(fixture.store, current,
                                                   fcr_test::reference_from_views(*views));
    }
  }
}

FCR_TEST(property, a_rejected_request_never_changes_the_published_state) {
  const std::uint64_t base = fcr_test::current_seed();
  fcr_test::Rng rng(base);
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  for (std::size_t step = 0; step < 200; ++step) {
    fcr_test::set_case_context("step=" + fcr::format_unsigned(static_cast<std::uint64_t>(step)));
    const std::string id = name_of("res", rng.below(1'000));
    fcr::ReserveRequest request = fcr_test::reserve_request(
        fixture, id.c_str(), name_of("a", rng.next()).c_str(),
        {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1 + rng.below(80))}, 1'100, 900'000);

    const fcr::Result<fcr::Revision> before = fixture.store.revision();
    FCR_REQUIRE(before.has_value());
    const fcr::Result<fcr::ReserveOutcome> outcome = fixture.store.reserve(request);
    const fcr::Result<fcr::Revision> after = fixture.store.revision();
    FCR_REQUIRE(after.has_value());
    if (outcome.has_value()) {
      FCR_CHECK_EQ(after->value(), before->value() + 1);
    } else {
      FCR_CHECK_EQ(after->value(), before->value());
    }
  }
}

FCR_TEST(property, the_accounting_is_never_above_the_reservable_capacity) {
  const std::uint64_t base = fcr_test::current_seed();
  fcr_test::Rng rng(base ^ 0x5DEECE66DULL);
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  for (std::size_t step = 0; step < 300; ++step) {
    const std::string id = name_of("res", step);
    fcr::ReserveRequest request = fcr_test::reserve_request(
        fixture, id.c_str(), name_of("a", step).c_str(),
        {fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 1 + rng.below(120'000))}, 1'100, 900'000);
    (void)fixture.store.reserve(request);
    const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
    FCR_REQUIRE(pools.has_value());
    for (const fcr::PoolAccount& account : *pools) {
      FCR_CHECK(account.committed <= account.reservable);
      FCR_CHECK(account.protected_ <= account.reservable);
      FCR_CHECK(account.committed + account.protected_ <= account.reservable);
      FCR_CHECK_EQ(account.committed + account.protected_ + account.free, account.reservable);
    }
  }
}

FCR_TEST(property, expiry_is_monotone_in_the_sweep_tick) {
  const std::uint64_t base = fcr_test::current_seed();
  fcr_test::Rng rng(base ^ 0xA5A5A5A5ULL);
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  std::size_t created = 0;
  for (int index = 0; index < 60; ++index) {
    const std::string id = name_of("res", static_cast<std::uint64_t>(index));
    const std::string attempt_name = name_of("a", static_cast<std::uint64_t>(index));
    const std::uint64_t deadline = 2'000 + rng.below(10'000);
    if (fixture.store
            .reserve(fcr_test::reserve_request(
                fixture, id.c_str(), attempt_name.c_str(),
                {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, deadline))
            .has_value()) {
      ++created;
    }
  }
  std::size_t expired_total = 0;
  for (std::uint64_t tick = 2'000; tick <= 14'000; tick += 500) {
    const fcr::Result<fcr::ExpireOutcome> outcome =
        fixture.store.expire(fcr_test::expire_request(fixture, name_of("sweep", tick).c_str(), tick));
    FCR_REQUIRE(outcome.has_value());
    expired_total += outcome->expired.size();
  }
  FCR_CHECK_EQ(expired_total, created);
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ((*pools)[0].committed, 0ULL);
  FCR_CHECK_EQ((*pools)[0].free, (*pools)[0].reservable);
}

FCR_TEST(property, state_documents_are_byte_identical_for_identical_states) {
  // The store incarnation is the only field that legitimately differs between
  // two independently created stores; masking it leaves a document that is a
  // pure function of the authoritative state.
  std::string reference;
  for (int variant = 0; variant < 4; ++variant) {
    fcr_test::TempDir directory("property_canonical");
    const std::filesystem::path root = directory / "store";
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(fcr_test::make_snapshot("snap-p", "site-p", 1, 1'000, kPools),
                                             fcr::Tick(1'000)));
    fcr_test::Fixture fixture;
    fixture.snapshot = fcr_test::make_snapshot("snap-p", "site-p", 1, 1'000, kPools);
    fixture.epoch = created->epoch().value();
    for (int index = 0; index < 8; ++index) {
      const std::string id = name_of("res", static_cast<std::uint64_t>(index));
      const std::string attempt_name = name_of("a", static_cast<std::uint64_t>(index));
      FCR_REQUIRE_OK(created->reserve(fcr_test::reserve_request(
          fixture, id.c_str(), attempt_name.c_str(),
          {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1 + static_cast<std::uint64_t>(index))}, 1'100, 900'000)));
    }
    std::string document = fcr_test::read_text_file(fcr_test::committed_state_file(root));
    // The store incarnation and the integrity digest that covers it are the only
    // fields that legitimately differ between two independently created stores.
    // Masking both leaves a document that is a pure function of the state.
    fcr_test::patch_text(document, "incarnation=" + created->incarnation().value().to_string(),
                         "incarnation=00000000000000000000000000000000");
    const std::size_t seal = document.find("digest=sha256:");
    FCR_REQUIRE(seal != std::string::npos);
    const std::size_t first_digit = seal + std::string("digest=sha256:").size();
    FCR_REQUIRE(first_digit + fcr::Sha256::kDigestHexDigits <= document.size());
    document.replace(first_digit, fcr::Sha256::kDigestHexDigits,
                     std::string(fcr::Sha256::kDigestHexDigits, '0'));
    const std::string digest = fcr::sha256_hex(document);
    if (variant == 0) {
      reference = digest;
    } else {
      FCR_CHECK_EQ(digest, reference);
    }
    created->close();
  }
}

FCR_TEST(property, a_claim_set_survives_a_document_round_trip_unchanged) {
  const std::uint64_t base = fcr_test::current_seed();
  fcr_test::Rng rng(base ^ 0xDEADBEEFULL);
  for (int variant = 0; variant < 20; ++variant) {
    fcr_test::TempDir directory("property_roundtrip");
    const std::filesystem::path root = directory / "store";
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(created->install_capacity(fcr_test::make_snapshot("snap-r", "site-r", 1, 1'000, kPools),
                                             fcr::Tick(1'000)));
    fcr_test::Fixture fixture;
    fixture.snapshot = fcr_test::make_snapshot("snap-r", "site-r", 1, 1'000, kPools);
    fixture.epoch = created->epoch().value();

    std::vector<fcr::ResourceClaim> claims;
    for (const fcr::ResourceKind kind : kKinds) {
      if (rng.coin()) {
        claims.push_back(fcr_test::claim(kind, scope_of(kind), 1 + rng.below(3)));
      }
    }
    if (claims.empty()) {
      claims.push_back(fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1));
    }
    FCR_REQUIRE_OK(created->reserve(fcr_test::reserve_request(
        fixture, "res-round", "attempt-round", claims, 1'100, 900'000)));
    created->close();

    fcr::Result<fcr::Store> reopened = fcr::Store::open(root);
    FCR_REQUIRE(reopened.has_value());
    const fcr::Result<std::optional<fcr::ReservationView>> view =
        reopened->find(fcr_test::reservation_id("res-round"));
    FCR_REQUIRE(view.has_value());
    FCR_REQUIRE(view->has_value());
    FCR_REQUIRE((*view)->record.claims.size() == claims.size());
    for (const fcr::ResourceClaim& expected : claims) {
      const fcr::ResourceClaim* found = nullptr;
      for (const fcr::ResourceClaim& actual : (*view)->record.claims) {
        if (actual.pool == expected.pool) {
          found = &actual;
        }
      }
      FCR_REQUIRE(found != nullptr);
      FCR_CHECK_EQ(found->amount, expected.amount);
    }
    reopened->close();
  }
}
