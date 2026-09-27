// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// An independent consumer of the installed Facility Capacity Reservation
// package. It exercises a minimal but real lifecycle through the exported
// target only, and exits non-zero if any step does not behave as documented.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <dccp/facility_capacity_reservation/capacity.hpp>
#include <dccp/facility_capacity_reservation/request.hpp>
#include <dccp/facility_capacity_reservation/status.hpp>
#include <dccp/facility_capacity_reservation/store.hpp>
#include <dccp/facility_capacity_reservation/version.hpp>

namespace fcr = dccp::facility_capacity_reservation;

namespace {

int fail(const std::string& what, const fcr::Error& error) {
  std::cerr << "consumer failed at " << what << ": " << error.to_string() << '\n';
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: consumer <store-directory>\n";
    return 2;
  }
  const std::filesystem::path root(argv[1]);
  std::error_code error;
  std::filesystem::remove_all(root, error);

  std::cout << "linked facility_capacity_reservation " << fcr::kVersionString << '\n';

  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  if (!created.has_value()) {
    return fail("create", created.error());
  }
  fcr::Store store = std::move(created).value();

  fcr::CapacitySnapshot snapshot;
  snapshot.ref = fcr::SnapshotRef::parse("snap-consumer", "snapshot ref").value();
  snapshot.facility = fcr::FacilityRef::parse("site-consumer", "facility ref").value();
  snapshot.source_generation = fcr::SourceGeneration(1);
  snapshot.captured_at_tick = fcr::Tick(1'000);
  snapshot.pools = {
      fcr::CapacityPool{{fcr::ResourceKind::Rack, fcr::ScopeRef::parse("hall-c", "scope").value()}, 20, 0, 2},
      fcr::CapacityPool{{fcr::ResourceKind::Power, fcr::ScopeRef::parse("feed-c", "scope").value()}, 200'000, 0, 0},
  };

  {
    const fcr::Result<void> installed = store.install_capacity(snapshot, fcr::Tick(1'000));
    if (!installed.has_value()) {
      return fail("install_capacity", installed.error());
    }
  }

  const fcr::Result<fcr::AuthorityEpoch> epoch = store.epoch();
  if (!epoch.has_value()) {
    return fail("epoch", epoch.error());
  }

  fcr::ReserveRequest reserve;
  reserve.attempt = fcr::AttemptId::parse("attempt-consumer-1", "attempt").value();
  reserve.id = fcr::ReservationId::parse("res-consumer", "reservation id").value();
  reserve.claimant = fcr::ClaimantRef::parse("consumer-team", "claimant").value();
  reserve.tenant = fcr::TenantRef::parse("consumer-tenant", "tenant").value();
  reserve.service = fcr::ServiceRef::parse("consumer-service", "service").value();
  reserve.priority = fcr::PriorityRef::parse("consumer-priority", "priority").value();
  reserve.headroom = fcr::HeadroomClass::Guaranteed;
  reserve.authority.epoch = *epoch;
  reserve.expected_source_generation = snapshot.source_generation;
  reserve.validity.start = fcr::Tick(1'100);
  reserve.validity.deadline = fcr::Tick(900'000);
  reserve.now = fcr::Tick(1'100);
  reserve.claims = {
      fcr::ResourceClaim{{fcr::ResourceKind::Rack, fcr::ScopeRef::parse("hall-c", "scope").value()}, 6},
      fcr::ResourceClaim{{fcr::ResourceKind::Power, fcr::ScopeRef::parse("feed-c", "scope").value()}, 60'000},
  };

  {
    const fcr::Result<fcr::ReserveOutcome> outcome = store.reserve(reserve);
    if (!outcome.has_value()) {
      return fail("reserve", outcome.error());
    }
    std::cout << "reserved at revision " << outcome->revision.value() << '\n';
  }

  {
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
      std::cout << account.pool.to_string() << " committed=" << account.committed << " free=" << account.free
                << '\n';
    }
  }

  store.close();

  fcr::Result<fcr::Store> reopened = fcr::Store::open(root);
  if (!reopened.has_value()) {
    return fail("open", reopened.error());
  }
  {
    const fcr::Result<fcr::VerificationReport> report = reopened->verify();
    if (!report.has_value() || !report->ok) {
      return fail("verify", report.has_value() ? fcr::Error(fcr::ErrorCode::AccountingMismatch, "not ok")
                                               : report.error());
    }
    std::cout << "reopened and verified " << report->reservation_count << " reservation(s)\n";
  }
  reopened->close();

  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
  if (cleanup) {
    std::cerr << "cannot clean up the consumer store\n";
    return 1;
  }
  std::cout << "consumer ok\n";
  return 0;
}
