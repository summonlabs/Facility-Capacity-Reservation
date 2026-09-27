// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durable lifecycle: create a store directory, commit reservations, close it,
// reopen it and prove that
//
//   * the committed state is exactly what was published,
//   * the writer authority epoch advanced, so a decision taken under the old
//     epoch is refused rather than merged, and
//   * the restored capacity evidence is not silently treated as current.
//
// The example writes only inside the directory it is given and never removes
// anything it did not create. It is SYNTHETIC: no real facility is involved.

#include <cstdint>
#include <filesystem>
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

fcr::CapacitySnapshot make_snapshot(std::uint64_t generation) {
  fcr::CapacitySnapshot snapshot;
  snapshot.ref = fcr::SnapshotRef::parse("snap-durable", "snapshot ref").value();
  snapshot.facility = fcr::FacilityRef::parse("site-alpha", "facility ref").value();
  snapshot.source_generation = fcr::SourceGeneration(generation);
  snapshot.captured_at_tick = fcr::Tick(1'000);
  snapshot.pools = {fcr::CapacityPool{
      {fcr::ResourceKind::Rack, fcr::ScopeRef::parse("hall-a", "scope").value()}, 64, 0, 8}};
  return snapshot;
}

void print_pools(const fcr::Store& store) {
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = store.pools();
  if (!pools.has_value()) {
    return;
  }
  for (const fcr::PoolAccount& account : *pools) {
    std::cout << "  " << account.pool.to_string() << ": reservable=" << account.reservable
              << " committed=" << account.committed << " protected=" << account.protected_
              << " free=" << account.free << " active=" << account.active_reservations << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: durable_lifecycle <store-directory>\n";
    return 2;
  }
  const std::filesystem::path root(argv[1]);
  std::error_code error;
  if (std::filesystem::exists(root, error) && !std::filesystem::is_empty(root, error)) {
    std::cerr << "refusing to use a directory that is not empty: " << root.string() << '\n';
    return 2;
  }

  // --- Create and commit --------------------------------------------------
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  if (!created.has_value()) {
    return fail("create", created.error());
  }
  fcr::Store store = std::move(created).value();

  {
    const fcr::Result<void> installed = store.install_capacity(make_snapshot(3), fcr::Tick(1'000));
    if (!installed.has_value()) {
      return fail("install_capacity", installed.error());
    }
  }

  const fcr::Result<fcr::AuthorityEpoch> epoch = store.epoch();
  if (!epoch.has_value()) {
    return fail("epoch", epoch.error());
  }
  const fcr::Result<fcr::Incarnation> incarnation = store.incarnation();
  if (!incarnation.has_value()) {
    return fail("incarnation", incarnation.error());
  }
  std::cout << "store " << root.string() << " incarnation " << incarnation->to_string() << " epoch "
            << epoch->value() << '\n';

  fcr::ReserveRequest reserve;
  reserve.attempt = fcr::AttemptId::parse("attempt-1", "attempt").value();
  reserve.id = fcr::ReservationId::parse("res-durable-1", "reservation id").value();
  reserve.claimant = fcr::ClaimantRef::parse("team-a", "claimant").value();
  reserve.tenant = fcr::TenantRef::parse("tenant-a", "tenant").value();
  reserve.service = fcr::ServiceRef::parse("svc-a", "service").value();
  reserve.priority = fcr::PriorityRef::parse("prio-normal", "priority").value();
  reserve.headroom = fcr::HeadroomClass::Guaranteed;
  reserve.authority.epoch = *epoch;
  reserve.expected_source_generation = fcr::SourceGeneration(3);
  reserve.validity.start = fcr::Tick(1'000);
  reserve.validity.deadline = fcr::Tick(1'000'000);
  reserve.now = fcr::Tick(1'000);
  reserve.claims = {fcr::ResourceClaim{
      {fcr::ResourceKind::Rack, fcr::ScopeRef::parse("hall-a", "scope").value()}, 16}};

  {
    const fcr::Result<fcr::ReserveOutcome> outcome = store.reserve(reserve);
    if (!outcome.has_value()) {
      return fail("reserve", outcome.error());
    }
    std::cout << "committed at revision " << outcome->revision.value() << '\n';
  }
  print_pools(store);

  const fcr::Result<fcr::Revision> before_close = store.revision();
  if (!before_close.has_value()) {
    return fail("revision", before_close.error());
  }
  store.close();

  // --- Reopen -------------------------------------------------------------
  fcr::Result<fcr::Store> reopened = fcr::Store::open(root);
  if (!reopened.has_value()) {
    return fail("open", reopened.error());
  }
  fcr::Store second = std::move(reopened).value();
  const fcr::RecoveryReport recovery = second.recovery();
  std::cout << "reopened: epoch " << recovery.previous_epoch.value() << " -> "
            << recovery.current_epoch.value() << ", revision " << recovery.revision.value()
            << ", capacity restored=" << recovery.capacity_restored << '\n';

  const fcr::Result<fcr::Revision> after_open = second.revision();
  if (!after_open.has_value() || after_open->value() != before_close->value()) {
    std::cerr << "the reopened store does not hold the committed revision\n";
    return 1;
  }
  print_pools(second);

  // --- A decision taken under the previous epoch is refused ---------------
  fcr::ReserveRequest stale = reserve;
  stale.attempt = fcr::AttemptId::parse("attempt-2", "attempt").value();
  stale.id = fcr::ReservationId::parse("res-durable-2", "reservation id").value();
  stale.authority.epoch = *epoch;  // the epoch the first handle held
  const fcr::Result<fcr::ReserveOutcome> refused = second.reserve(stale);
  if (refused.has_value()) {
    std::cerr << "a decision taken under a superseded epoch was accepted\n";
    return 1;
  }
  std::cout << "stale authority refused: " << fcr::error_code_name(refused.error().code()) << '\n';

  // Restored capacity evidence is not current, so consuming operations refuse it
  // until a fresh snapshot is reconciled.
  stale.authority.epoch = recovery.current_epoch;
  const fcr::Result<fcr::ReserveOutcome> stale_capacity = second.reserve(stale);
  if (stale_capacity.has_value()) {
    std::cerr << "restored capacity evidence was silently treated as current\n";
    return 1;
  }
  std::cout << "restored capacity refused: " << fcr::error_code_name(stale_capacity.error().code()) << '\n';

  fcr::ReconcileRequest reconcile;
  reconcile.attempt = fcr::AttemptId::parse("attempt-reconcile", "attempt").value();
  reconcile.snapshot = make_snapshot(4);
  reconcile.expected_source_generation = fcr::SourceGeneration(3);
  reconcile.mode = fcr::ReconcileMode::Enforce;
  reconcile.actor = fcr::ActorRef::parse("facility-authority", "actor").value();
  reconcile.authority.epoch = recovery.current_epoch;
  reconcile.now = fcr::Tick(2'000);

  {
    const fcr::Result<fcr::ReconcileOutcome> outcome = second.reconcile(reconcile);
    if (!outcome.has_value()) {
      return fail("reconcile", outcome.error());
    }
    std::cout << "reconciled to source generation " << outcome->source_generation.value() << " (from "
              << outcome->previous_source_generation.value() << ")\n";
  }

  const fcr::Result<fcr::LedgerStatus> status = second.status();
  if (!status.has_value()) {
    return fail("status", status.error());
  }
  std::cout << "capacity is now fresh=" << status->capacity_fresh << " at generation "
            << status->source_generation.value() << '\n';

  const fcr::Result<fcr::VerificationReport> verified = second.verify();
  if (!verified.has_value() || !verified->ok) {
    return fail("verify", verified.has_value() ? fcr::Error(fcr::ErrorCode::AccountingMismatch, "not ok")
                                               : verified.error());
  }
  std::cout << "verify: " << verified->pool_count << " pools closed exactly\n";
  second.close();
  return 0;
}
