// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Basic reservation: consume a capacity snapshot, commit a multi-resource
// reservation, amend it, release it, and check that the accounting closes at
// every step.
//
// The example runs against an in-memory store, so it needs no directory and
// leaves nothing behind. It is SYNTHETIC: the capacity figures are invented and
// no real facility is involved.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "dccp/facility_capacity_reservation/capacity.hpp"
#include "dccp/facility_capacity_reservation/ledger.hpp"
#include "dccp/facility_capacity_reservation/request.hpp"
#include "dccp/facility_capacity_reservation/status.hpp"
#include "dccp/facility_capacity_reservation/store.hpp"
#include "dccp/facility_capacity_reservation/text.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

int fail(const std::string& what, const fcr::Error& error) {
  std::cerr << "example failed at " << what << ": " << error.to_string() << '\n';
  return 1;
}

fcr::Tick tick(std::uint64_t value) { return fcr::Tick(value); }

}  // namespace

int main() {
  // --- Consume capacity evidence ------------------------------------------
  fcr::CapacitySnapshot snapshot;
  snapshot.ref = fcr::SnapshotRef::parse("snap-0001", "snapshot ref").value();
  snapshot.facility = fcr::FacilityRef::parse("site-alpha", "facility ref").value();
  snapshot.source_generation = fcr::SourceGeneration(7);
  snapshot.captured_at_tick = tick(1'000);
  snapshot.pools = {
      fcr::CapacityPool{{fcr::ResourceKind::Rack, fcr::ScopeRef::parse("hall-a", "scope").value()}, 40, 4, 6},
      fcr::CapacityPool{{fcr::ResourceKind::Power, fcr::ScopeRef::parse("feed-a", "scope").value()},
                        2'000'000, 0, 100'000},
      fcr::CapacityPool{{fcr::ResourceKind::Cooling, fcr::ScopeRef::parse("loop-a", "scope").value()},
                        1'800'000, 0, 0},
  };

  fcr::Result<fcr::Store> opened = fcr::Store::in_memory();
  if (!opened.has_value()) {
    std::cerr << "cannot create an in-memory store: " << opened.error().to_string() << '\n';
    return 1;
  }
  fcr::Store store = std::move(opened).value();

  {
    const fcr::Result<void> installed = store.install_capacity(snapshot, tick(1'000));
    if (!installed.has_value()) {
      return fail("install_capacity", installed.error());
    }
  }

  const fcr::Result<fcr::AuthorityEpoch> epoch = store.epoch();
  if (!epoch.has_value()) {
    return fail("epoch", epoch.error());
  }

  // --- Commit a multi-resource reservation --------------------------------
  fcr::ReserveRequest reserve;
  reserve.attempt = fcr::AttemptId::parse("attempt-reserve-1", "attempt").value();
  reserve.id = fcr::ReservationId::parse("res-0001", "reservation id").value();
  reserve.claimant = fcr::ClaimantRef::parse("team-search", "claimant").value();
  reserve.tenant = fcr::TenantRef::parse("tenant-acme", "tenant").value();
  reserve.service = fcr::ServiceRef::parse("svc-training", "service").value();
  reserve.priority = fcr::PriorityRef::parse("prio-high", "priority").value();
  reserve.headroom = fcr::HeadroomClass::Firm;
  reserve.authority.epoch = *epoch;
  reserve.expected_source_generation = snapshot.source_generation;
  reserve.validity.start = tick(1'100);
  reserve.validity.deadline = tick(50'000);
  reserve.now = tick(1'100);
  reserve.claims = {
      fcr::ResourceClaim{{fcr::ResourceKind::Rack, fcr::ScopeRef::parse("hall-a", "scope").value()}, 8},
      fcr::ResourceClaim{{fcr::ResourceKind::Power, fcr::ScopeRef::parse("feed-a", "scope").value()}, 320'000},
      fcr::ResourceClaim{{fcr::ResourceKind::Cooling, fcr::ScopeRef::parse("loop-a", "scope").value()}, 300'000},
  };

  const fcr::Result<fcr::ReserveOutcome> reserved = store.reserve(reserve);
  if (!reserved.has_value()) {
    return fail("reserve", reserved.error());
  }
  std::cout << "reserved " << reserved->reservation.id().value() << " generation "
            << reserved->reservation.record.generation.value() << " at revision " << reserved->revision.value() << '\n';

  // The same attempt identity replays instead of reserving twice.
  const fcr::Result<fcr::ReserveOutcome> replayed = store.reserve(reserve);
  if (!replayed.has_value() || !replayed->replayed) {
    return fail("reserve replay", replayed.has_value() ? fcr::Error(fcr::ErrorCode::InternalError, "not replayed")
                                                       : replayed.error());
  }
  std::cout << "the repeated attempt replayed instead of reserving twice\n";

  // --- Amend it -----------------------------------------------------------
  fcr::AmendRequest amend;
  amend.attempt = fcr::AttemptId::parse("attempt-amend-1", "attempt").value();
  amend.id = reserve.id;
  amend.expected_generation = reserved->reservation.record.generation;
  amend.actor = fcr::ActorRef::parse("team-search", "actor").value();
  amend.authority.epoch = *epoch;
  amend.expected_source_generation = snapshot.source_generation;
  amend.validity.start = tick(1'100);
  amend.validity.deadline = tick(90'000);
  amend.now = tick(2'000);
  amend.cause = fcr::AmendmentCause::CapacityIncrease;
  amend.detail = "grew the training slice";
  amend.claims = {
      fcr::ResourceClaim{{fcr::ResourceKind::Rack, fcr::ScopeRef::parse("hall-a", "scope").value()}, 12},
      fcr::ResourceClaim{{fcr::ResourceKind::Power, fcr::ScopeRef::parse("feed-a", "scope").value()}, 480'000},
      fcr::ResourceClaim{{fcr::ResourceKind::Cooling, fcr::ScopeRef::parse("loop-a", "scope").value()}, 450'000},
  };

  const fcr::Result<fcr::AmendOutcome> amended = store.amend(amend);
  if (!amended.has_value()) {
    return fail("amend", amended.error());
  }
  std::cout << "amended to generation " << amended->reservation.record.generation.value() << " (was "
            << amended->previous_generation.value() << "), lineage length "
            << amended->reservation.record.lineage.size() << '\n';

  // --- Accounting closes exactly ------------------------------------------
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = store.pools();
  if (!pools.has_value()) {
    return fail("pools", pools.error());
  }
  for (const fcr::PoolAccount& account : *pools) {
    const fcr::CapacitySplit split{fcr::Quantity(account.free), fcr::Quantity(account.committed),
                                   fcr::Quantity(account.protected_)};
    const fcr::Result<fcr::Quantity> accounted = split.accounted();
    if (!accounted.has_value() || accounted->units() != account.reservable) {
      std::cerr << "accounting does not close for " << account.pool.to_string() << '\n';
      return 1;
    }
    std::cout << account.pool.to_string() << ": reservable=" << account.reservable
              << " committed=" << account.committed << " protected=" << account.protected_
              << " free=" << account.free << '\n';
  }

  // --- Release it ---------------------------------------------------------
  fcr::ReleaseRequest release;
  release.attempt = fcr::AttemptId::parse("attempt-release-1", "attempt").value();
  release.id = reserve.id;
  release.expected_generation = amended->reservation.record.generation;
  release.actor = amend.actor;
  release.authority.epoch = *epoch;
  release.detail = "workload finished early";
  release.now = tick(3'000);

  const fcr::Result<fcr::ReleaseOutcome> released = store.release(release);
  if (!released.has_value()) {
    return fail("release", released.error());
  }
  std::cout << "released " << released->reservation.id().value() << " with cause "
            << fcr::transition_cause_token(released->reservation.record.termination->cause) << ", returned "
            << released->released.size() << " claims to free\n";

  const fcr::Result<fcr::VerificationReport> verified = store.verify();
  if (!verified.has_value() || !verified->ok) {
    return fail("verify", verified.has_value() ? fcr::Error(fcr::ErrorCode::AccountingMismatch, "not ok")
                                               : verified.error());
  }
  std::cout << "verify: " << verified->pool_count << " pools closed exactly across "
            << verified->reservation_count << " reservations\n";
  return 0;
}
