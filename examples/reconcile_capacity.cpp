// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Reconciling withdrawn capacity: a facility loses rack and power capacity, the
// new snapshot is reconciled in observation mode first, then enforced. The
// example shows the deterministic fencing order (opportunistic before firm,
// earliest deadline first), that guaranteed commitments are never fenced
// automatically, and that the accounting closes again afterwards.
//
// SYNTHETIC: the capacity figures and the withdrawal are invented.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "dccp/facility_capacity_reservation/capacity.hpp"
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

fcr::ScopeRef scope(const char* text) { return fcr::ScopeRef::parse(text, "scope").value(); }

fcr::CapacitySnapshot snapshot(std::uint64_t generation, std::uint64_t racks) {
  fcr::CapacitySnapshot value;
  value.ref = fcr::SnapshotRef::parse("snap-reconcile", "snapshot ref").value();
  value.facility = fcr::FacilityRef::parse("site-beta", "facility ref").value();
  value.source_generation = fcr::SourceGeneration(generation);
  value.captured_at_tick = fcr::Tick(1'000 * generation);
  value.pools = {fcr::CapacityPool{{fcr::ResourceKind::Rack, scope("hall-b")}, racks, 0, 0}};
  return value;
}

struct ReservationSpec {
  const char* id;
  fcr::HeadroomClass headroom;
  std::uint64_t racks;
  std::uint64_t deadline;
};

}  // namespace

int main() {
  fcr::Result<fcr::Store> opened = fcr::Store::in_memory();
  if (!opened.has_value()) {
    return fail("in_memory", opened.error());
  }
  fcr::Store store = std::move(opened).value();

  {
    const fcr::Result<void> installed = store.install_capacity(snapshot(1, 40), fcr::Tick(1'000));
    if (!installed.has_value()) {
      return fail("install_capacity", installed.error());
    }
  }

  const fcr::Result<fcr::AuthorityEpoch> epoch = store.epoch();
  if (!epoch.has_value()) {
    return fail("epoch", epoch.error());
  }

  const ReservationSpec specs[] = {
      {"res-guaranteed", fcr::HeadroomClass::Guaranteed, 10, 900'000},
      {"res-firm-late", fcr::HeadroomClass::Firm, 8, 800'000},
      {"res-firm-early", fcr::HeadroomClass::Firm, 6, 400'000},
      {"res-opportunistic", fcr::HeadroomClass::Opportunistic, 4, 700'000},
  };

  std::uint64_t counter = 0;
  for (const ReservationSpec& spec : specs) {
    ++counter;
    fcr::ReserveRequest request;
    request.attempt = fcr::AttemptId::parse("attempt-" + std::to_string(counter), "attempt").value();
    request.id = fcr::ReservationId::parse(spec.id, "reservation id").value();
    request.claimant = fcr::ClaimantRef::parse("team-x", "claimant").value();
    request.tenant = fcr::TenantRef::parse("tenant-x", "tenant").value();
    request.service = fcr::ServiceRef::parse("svc-x", "service").value();
    request.priority = fcr::PriorityRef::parse("prio-normal", "priority").value();
    request.headroom = spec.headroom;
    request.authority.epoch = *epoch;
    request.expected_source_generation = fcr::SourceGeneration(1);
    request.validity.start = fcr::Tick(1'000);
    request.validity.deadline = fcr::Tick(spec.deadline);
    request.now = fcr::Tick(1'000);
    request.claims = {fcr::ResourceClaim{{fcr::ResourceKind::Rack, scope("hall-b")}, spec.racks}};
    const fcr::Result<fcr::ReserveOutcome> outcome = store.reserve(request);
    if (!outcome.has_value()) {
      return fail(std::string("reserve ") + spec.id, outcome.error());
    }
  }

  const fcr::Result<std::vector<fcr::PoolAccount>> before = store.pools();
  if (!before.has_value()) {
    return fail("pools", before.error());
  }
  for (const fcr::PoolAccount& account : *before) {
    std::cout << "before: " << account.pool.to_string() << " reservable=" << account.reservable
              << " committed=" << account.committed << " protected=" << account.protected_
              << " free=" << account.free << '\n';
  }

  // --- Observation first --------------------------------------------------
  fcr::ReconcileRequest observe;
  observe.attempt = fcr::AttemptId::parse("attempt-observe", "attempt").value();
  observe.snapshot = snapshot(2, 26);
  observe.expected_source_generation = fcr::SourceGeneration(1);
  observe.mode = fcr::ReconcileMode::Observe;
  observe.actor = fcr::ActorRef::parse("facility-authority", "actor").value();
  observe.authority.epoch = *epoch;
  observe.now = fcr::Tick(2'000);

  const fcr::Result<fcr::ReconcileOutcome> observed = store.reconcile(observe);
  if (!observed.has_value()) {
    return fail("reconcile observe", observed.error());
  }
  std::cout << "observation adopted=" << observed->adopted << " overcommits="
            << observed->overcommits.size() << '\n';
  for (const fcr::PoolOvercommit& overcommit : observed->overcommits) {
    std::cout << "  overcommit " << overcommit.pool.to_string() << " reservable=" << overcommit.reservable
              << " committed=" << overcommit.committed << " protected=" << overcommit.protected_
              << " excess=" << overcommit.excess << '\n';
  }

  // --- Enforce ------------------------------------------------------------
  fcr::ReconcileRequest enforce = observe;
  enforce.attempt = fcr::AttemptId::parse("attempt-enforce", "attempt").value();
  enforce.mode = fcr::ReconcileMode::Enforce;
  enforce.policy = fcr::PolicyRef::parse("policy-withdrawal", "policy").value();

  const fcr::Result<fcr::ReconcileOutcome> enforced = store.reconcile(enforce);
  if (!enforced.has_value()) {
    return fail("reconcile enforce", enforced.error());
  }
  std::cout << "enforced: adopted=" << enforced->adopted << " fenced=" << enforced->fenced.size() << '\n';
  for (const fcr::FencedReservation& fenced : enforced->fenced) {
    std::cout << "  fenced " << fenced.id.value() << " (" << fcr::headroom_class_token(fenced.headroom) << ")\n";
  }

  const fcr::Result<std::vector<fcr::PoolAccount>> after = store.pools();
  if (!after.has_value()) {
    return fail("pools", after.error());
  }
  for (const fcr::PoolAccount& account : *after) {
    std::cout << "after: " << account.pool.to_string() << " reservable=" << account.reservable
              << " committed=" << account.committed << " protected=" << account.protected_
              << " free=" << account.free << " active=" << account.active_reservations << '\n';
    const fcr::CapacitySplit split{fcr::Quantity(account.free), fcr::Quantity(account.committed),
                                   fcr::Quantity(account.protected_)};
    const fcr::Result<fcr::Quantity> accounted = split.accounted();
    if (!accounted.has_value() || accounted->units() != account.reservable) {
      std::cerr << "the accounting does not close after reconciliation\n";
      return 1;
    }
  }

  // --- What is still stale ------------------------------------------------
  const fcr::Result<fcr::Revision> revision = store.revision();
  if (!revision.has_value()) {
    return fail("revision", revision.error());
  }
  fcr::RevalidateRequest revalidate;
  revalidate.expected_revision = *revision;
  revalidate.now = fcr::Tick(2'000);
  const fcr::Result<fcr::RevalidationReport> report = store.revalidate(revalidate);
  if (!report.has_value()) {
    return fail("revalidate", report.error());
  }
  std::cout << "revalidate: current=" << report->current_count << " stale=" << report->stale_count
            << " capacity-exceeded=" << report->capacity_exceeded_count << '\n';
  return 0;
}
