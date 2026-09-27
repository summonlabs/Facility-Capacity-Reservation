// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Completed-operation benchmarks.
//
// What is measured is the wall-clock time of whole operations that actually
// completed, including the durability cost of the operations that are durable:
// a "durable reserve" figure includes staging the new state generation, flushing
// it, reading it back to verify it and committing the head pointer, because that
// is what the call does. A "volatile reserve" figure measures the same operation
// with no persistence at all, and the difference between the two is the cost of
// durability, not a speed-up.
//
// The workloads are SYNTHETIC: the capacity figures and the reservation shapes
// are invented, and no facility, device or network is involved. Every benchmark
// verifies the accounting closure afterwards and removes the store directory it
// created.
//
// Methodology notes:
//   * each benchmark is run in one process, in a fixed order, with no other
//     work in flight;
//   * the reported figure is the mean over the measured repetitions after a
//     warm-up that is not measured;
//   * the median is reported alongside the mean so that a single slow
//     filesystem flush cannot be mistaken for the typical cost;
//   * no benchmark is compared against a previous build here: a before/after
//     pair would only be meaningful with controlled alternating runs.

#include <algorithm>
#include <chrono>
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
#include "dccp/facility_capacity_reservation/version.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "benchmark check failed: " << what << '\n';
    ++g_failures;
  }
}

const std::vector<fcr::CapacityPool>& pool_layout() {
  static const std::vector<fcr::CapacityPool> pools = [] {
    std::vector<fcr::CapacityPool> value;
    value.push_back(fcr::CapacityPool{
        {fcr::ResourceKind::Rack, fcr::ScopeRef::parse("hall-bench", "scope").value()}, 20'000, 0, 0});
    value.push_back(fcr::CapacityPool{
        {fcr::ResourceKind::Power, fcr::ScopeRef::parse("feed-bench", "scope").value()}, 20'000'000, 0, 0});
    return value;
  }();
  return pools;
}

fcr::CapacitySnapshot make_snapshot(std::uint64_t generation) {
  fcr::CapacitySnapshot snapshot;
  snapshot.ref = fcr::SnapshotRef::parse("snap-bench", "snapshot ref").value();
  snapshot.facility = fcr::FacilityRef::parse("site-bench", "facility ref").value();
  snapshot.source_generation = fcr::SourceGeneration(generation);
  snapshot.captured_at_tick = fcr::Tick(1'000);
  snapshot.pools = pool_layout();
  return snapshot;
}

fcr::ReserveRequest make_request(const fcr::Store& store, const std::string& index) {
  fcr::ReserveRequest request;
  request.attempt = fcr::AttemptId::parse("attempt-" + index, "attempt").value();
  request.id = fcr::ReservationId::parse("res-" + index, "reservation id").value();
  request.claimant = fcr::ClaimantRef::parse("bench-claimant", "claimant").value();
  request.tenant = fcr::TenantRef::parse("bench-tenant", "tenant").value();
  request.service = fcr::ServiceRef::parse("bench-service", "service").value();
  request.priority = fcr::PriorityRef::parse("bench-priority", "priority").value();
  request.headroom = fcr::HeadroomClass::Firm;
  request.authority.epoch = store.epoch().value();
  request.expected_source_generation = store.status().value().source_generation;
  request.validity.start = fcr::Tick(2'000);
  request.validity.deadline = fcr::Tick(900'000);
  request.now = fcr::Tick(2'000);
  request.claims = {
      fcr::ResourceClaim{{fcr::ResourceKind::Rack, fcr::ScopeRef::parse("hall-bench", "scope").value()}, 1},
      fcr::ResourceClaim{{fcr::ResourceKind::Power, fcr::ScopeRef::parse("feed-bench", "scope").value()}, 1'000},
  };
  return request;
}

struct Timing {
  double mean_microseconds = 0;
  double median_microseconds = 0;
  std::size_t samples = 0;
};

template <class Operation>
Timing measure(std::size_t warmup, std::size_t repetitions, Operation&& operation) {
  for (std::size_t index = 0; index < warmup; ++index) {
    operation(index);
  }
  std::vector<double> samples;
  samples.reserve(repetitions);
  for (std::size_t index = 0; index < repetitions; ++index) {
    const auto started = std::chrono::steady_clock::now();
    operation(warmup + index);
    const auto finished = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration<double, std::micro>(finished - started).count());
  }
  std::sort(samples.begin(), samples.end());
  Timing timing;
  timing.samples = samples.size();
  double total = 0;
  for (const double sample : samples) {
    total += sample;
  }
  timing.mean_microseconds = samples.empty() ? 0 : total / static_cast<double>(samples.size());
  timing.median_microseconds = samples.empty() ? 0 : samples[samples.size() / 2];
  return timing;
}

void report(const std::string& name, const Timing& timing, const std::string& note) {
  std::cout << name << ": mean=" << timing.mean_microseconds << " us median=" << timing.median_microseconds
            << " us samples=" << timing.samples << " (" << note << ")\n";
}

/// Verifies the accounting closure and returns the number of active
/// reservations.
std::size_t verify_store(fcr::Store& store) {
  const fcr::Result<fcr::VerificationReport> report = store.verify();
  check(report.has_value() && report->ok, "verify() reports a closed accounting");
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = store.pools();
  check(pools.has_value(), "pools() succeeds");
  if (pools.has_value()) {
    for (const fcr::PoolAccount& account : *pools) {
      check(account.committed + account.protected_ + account.free == account.reservable,
            "committed + protected + free equals reservable for " + account.pool.to_string());
    }
  }
  const fcr::Result<fcr::LedgerStatus> status = store.status();
  check(status.has_value(), "status() succeeds");
  return status.has_value() ? status->active_count : 0;
}

void benchmark_volatile_reserve() {
  fcr::Result<fcr::Store> opened = fcr::Store::in_memory();
  check(opened.has_value(), "in-memory store");
  if (!opened.has_value()) {
    return;
  }
  fcr::Store& store = opened.value();
  check(store.install_capacity(make_snapshot(1), fcr::Tick(1'000)).has_value(), "install capacity");

  constexpr std::size_t kWarmup = 100;
  constexpr std::size_t kRepetitions = 500;
  const Timing timing = measure(kWarmup, kRepetitions, [&store](std::size_t index) {
    const fcr::Result<fcr::ReserveOutcome> outcome = store.reserve(make_request(store, fcr::format_unsigned(index)));
    check(outcome.has_value(), "volatile reserve succeeds");
  });
  report("reserve/volatile", timing, "SYNTHETIC, in-memory ledger, no persistence");
  const std::size_t active = verify_store(store);
  check(active == kWarmup + kRepetitions, "every volatile reservation is held");
  store.close();
}

void benchmark_volatile_lifecycle() {
  fcr::Result<fcr::Store> opened = fcr::Store::in_memory();
  check(opened.has_value(), "in-memory store");
  if (!opened.has_value()) {
    return;
  }
  fcr::Store& store = opened.value();
  check(store.install_capacity(make_snapshot(1), fcr::Tick(1'000)).has_value(), "install capacity");

  constexpr std::size_t kRepetitions = 1'000;
  // The reservation for iteration i is created before the measured window and
  // released inside it, so the figure is the cost of a completed release.
  const Timing timing = measure(50, kRepetitions, [&store](std::size_t index) {
    const std::string label = "life-" + fcr::format_unsigned(index);
    const fcr::Result<fcr::ReserveOutcome> reserved = store.reserve(make_request(store, label));
    check(reserved.has_value(), "lifecycle reserve succeeds");
    if (!reserved.has_value()) {
      return;
    }
    fcr::ReleaseRequest release;
    release.attempt = fcr::AttemptId::parse("release-" + label, "attempt").value();
    release.id = reserved->reservation.record.id;
    release.expected_generation = reserved->reservation.record.generation;
    release.actor = fcr::ActorRef::parse("bench-claimant", "actor").value();
    release.authority.epoch = store.epoch().value();
    release.now = fcr::Tick(3'000);
    const fcr::Result<fcr::ReleaseOutcome> outcome = store.release(release);
    check(outcome.has_value(), "lifecycle release succeeds");
  });
  report("reserve+release/volatile", timing, "SYNTHETIC, one completed reserve and release per sample");
  const std::size_t active = verify_store(store);
  check(active == 0, "every lifecycle reservation was released");
  store.close();
}

void benchmark_volatile_amend() {
  fcr::StoreOptions options;
  // The benchmark performs more amendments than the default lineage bound, so
  // the bound is raised explicitly rather than the workload being trimmed.
  options.ledger.limits.max_lineage_entries = 8'192;
  fcr::Result<fcr::Store> opened = fcr::Store::in_memory(options);
  check(opened.has_value(), "in-memory store");
  if (!opened.has_value()) {
    return;
  }
  fcr::Store& store = opened.value();
  check(store.install_capacity(make_snapshot(1), fcr::Tick(1'000)).has_value(), "install capacity");
  const fcr::Result<fcr::ReserveOutcome> reserved = store.reserve(make_request(store, "amend-target"));
  check(reserved.has_value(), "amend target exists");
  if (!reserved.has_value()) {
    return;
  }
  fcr::ReservationGeneration generation = reserved->reservation.record.generation;

  constexpr std::size_t kWarmup = 50;
  constexpr std::size_t kRepetitions = 1'000;
  const Timing timing = measure(kWarmup, kRepetitions, [&store, &generation](std::size_t index) {
    fcr::AmendRequest request;
    // A request may tighten a bound but never relax it, so the request's own
    // lineage bound has to allow the workload as well.
    request.limits.max_lineage_entries = 8'192;
    request.attempt = fcr::AttemptId::parse("amend-bench-" + fcr::format_unsigned(index), "attempt").value();
    request.id = fcr::ReservationId::parse("res-amend-target", "reservation id").value();
    request.expected_generation = generation;
    request.actor = fcr::ActorRef::parse("bench-claimant", "actor").value();
    request.authority.epoch = store.epoch().value();
    request.expected_source_generation = store.status().value().source_generation;
    request.validity.start = fcr::Tick(2'000);
    request.validity.deadline = fcr::Tick(900'000);
    request.now = fcr::Tick(4'000 + index);
    request.claims = {
        fcr::ResourceClaim{{fcr::ResourceKind::Rack, fcr::ScopeRef::parse("hall-bench", "scope").value()},
                           // Starts one unit above the reservation being amended so
                           // that the first amendment is not a no-op.
                           2 + (index % 8)},
        fcr::ResourceClaim{{fcr::ResourceKind::Power, fcr::ScopeRef::parse("feed-bench", "scope").value()}, 1'000},
    };
    const fcr::Result<fcr::AmendOutcome> outcome = store.amend(request);
    check(outcome.has_value(), "amend succeeds");
    if (outcome.has_value()) {
      generation = outcome->reservation.record.generation;
    }
  });
  report("amend/volatile", timing, "SYNTHETIC, one completed amendment per sample");
  const fcr::Result<std::optional<fcr::ReservationView>> view =
      store.find(fcr::ReservationId::parse("res-amend-target", "id").value());
  check(view.has_value() && view->has_value(), "the amended reservation is held");
  if (view.has_value() && view->has_value()) {
    check((*view)->record.lineage.size() == kWarmup + kRepetitions, "every amendment is in the lineage");
    check((*view)->record.generation.value() == 1 + kWarmup + kRepetitions,
          "the generation advanced once per amendment");
  }
  verify_store(store);
  store.close();
}

void benchmark_durable_reserve(const std::filesystem::path& root) {
  std::error_code error;
  std::filesystem::remove_all(root, error);

  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  check(created.has_value(), "durable store created");
  if (!created.has_value()) {
    return;
  }
  fcr::Store& store = created.value();
  check(store.install_capacity(make_snapshot(1), fcr::Tick(1'000)).has_value(), "install capacity");

  constexpr std::size_t kWarmup = 50;
  constexpr std::size_t kRepetitions = 500;
  const Timing timing = measure(kWarmup, kRepetitions, [&store](std::size_t index) {
    const fcr::Result<fcr::ReserveOutcome> outcome = store.reserve(make_request(store, fcr::format_unsigned(index)));
    check(outcome.has_value(), "durable reserve succeeds");
  });
  report("reserve/durable", timing,
         "SYNTHETIC, includes state encode, write, flush, read-back verify and head commit");
  const std::size_t active = verify_store(store);
  check(active == kWarmup + kRepetitions, "every durable reservation is held");
  store.close();
}

void benchmark_durable_reopen(const std::filesystem::path& root) {
  std::error_code error;
  std::filesystem::remove_all(root, error);
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    check(created.has_value(), "durable store created");
    if (!created.has_value()) {
      return;
    }
    check(created->install_capacity(make_snapshot(1), fcr::Tick(1'000)).has_value(), "install capacity");
    for (std::size_t index = 0; index < 300; ++index) {
      check(created->reserve(make_request(*created, fcr::format_unsigned(index))).has_value(),
            "reopen fixture reserve succeeds");
    }
    created->close();
  }

  constexpr std::size_t kWarmup = 5;
  constexpr std::size_t kRepetitions = 50;
  const Timing timing = measure(kWarmup, kRepetitions, [&root](std::size_t) {
    fcr::Result<fcr::Store> opened = fcr::Store::open(root);
    check(opened.has_value(), "reopen succeeds");
    if (!opened.has_value()) {
      return;
    }
    const fcr::Result<fcr::VerificationReport> report = opened->verify();
    check(report.has_value() && report->ok, "reopened store verifies");
    opened->close();
  });
  report("open+verify/durable", timing,
         "SYNTHETIC, includes lock, digest verification, decode, epoch handover and full verify");

  fcr::Result<fcr::Store> opened = fcr::Store::open(root);
  check(opened.has_value(), "final open succeeds");
  if (opened.has_value()) {
    const std::size_t active = verify_store(*opened);
    check(active == 300, "the reopened store holds every reservation");
    opened->close();
  }
}

void benchmark_durable_expire(const std::filesystem::path& root) {
  std::error_code error;
  std::filesystem::remove_all(root, error);
  {
    fcr::Result<fcr::Store> created = fcr::Store::create(root);
    check(created.has_value(), "durable store created");
    if (!created.has_value()) {
      return;
    }
    check(created->install_capacity(make_snapshot(1), fcr::Tick(1'000)).has_value(), "install capacity");
    created->close();
  }

  constexpr std::size_t kRepetitions = 40;
  constexpr std::size_t kBatch = 25;
  const Timing timing = measure(2, kRepetitions, [&root](std::size_t index) {
    fcr::Result<fcr::Store> opened = fcr::Store::open(root, [] {
      fcr::StoreOptions options;
      options.trust_persisted_capacity = true;
      return options;
    }());
    check(opened.has_value(), "expire fixture store opens");
    if (!opened.has_value()) {
      return;
    }
    const std::uint64_t base = static_cast<std::uint64_t>(index) * kBatch;
    for (std::size_t inner = 0; inner < kBatch; ++inner) {
      fcr::ReserveRequest request = make_request(*opened, fcr::format_unsigned(base + inner));
      request.validity.deadline = fcr::Tick(2'500);
      check(opened->reserve(request).has_value(), "expire fixture reserve succeeds");
    }
    fcr::ExpireRequest sweep;
    sweep.attempt = fcr::AttemptId::parse("sweep-" + fcr::format_unsigned(index), "attempt").value();
    sweep.authority.epoch = opened->epoch().value();
    sweep.now = fcr::Tick(3'000);
    const fcr::Result<fcr::ExpireOutcome> outcome = opened->expire(sweep);
    check(outcome.has_value(), "expire sweep succeeds");
    if (outcome.has_value()) {
      check(outcome->expired.size() == kBatch, "the sweep ends the whole batch");
    }
    opened->close();
  });
  report("expire-batch/durable", timing,
         "SYNTHETIC, one sweep of 25 commitments per sample, including its commit");
  fcr::Result<fcr::Store> opened = fcr::Store::open(root);
  check(opened.has_value(), "final expire store opens");
  if (opened.has_value()) {
    const std::size_t active = verify_store(*opened);
    check(active == 0, "every swept reservation is terminal");
    opened->close();
  }
}

void benchmark_verification(const std::filesystem::path& root) {
  std::error_code error;
  std::filesystem::remove_all(root, error);
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  check(created.has_value(), "durable store created");
  if (!created.has_value()) {
    return;
  }
  fcr::Store& store = created.value();
  check(store.install_capacity(make_snapshot(1), fcr::Tick(1'000)).has_value(), "install capacity");
  for (std::size_t index = 0; index < 300; ++index) {
    check(store.reserve(make_request(store, fcr::format_unsigned(index))).has_value(),
          "verification fixture reserve succeeds");
  }
  constexpr std::size_t kWarmup = 5;
  constexpr std::size_t kRepetitions = 60;
  const Timing timing = measure(kWarmup, kRepetitions, [&store](std::size_t) {
    const fcr::Result<fcr::VerificationReport> report = store.verify();
    check(report.has_value() && report->ok, "verify succeeds");
  });
  report("verify/300-reservations", timing,
         "SYNTHETIC, includes re-reading and re-decoding the committed artifact");
  store.close();
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path scratch;
  if (argc > 1) {
    scratch = std::filesystem::path(argv[1]);
  } else {
    std::error_code error;
    scratch = std::filesystem::temp_directory_path(error) / "fcr_benchmarks";
  }

  std::cout << "facility_capacity_reservation benchmarks " << fcr::kVersionString << '\n';
  std::cout << "scratch directory: " << scratch.string() << '\n';
  std::cout << "all workloads are SYNTHETIC: invented capacity, no facility hardware involved\n";

  benchmark_volatile_reserve();
  benchmark_volatile_lifecycle();
  benchmark_volatile_amend();
  benchmark_durable_reserve(scratch / "reserve");
  benchmark_durable_reopen(scratch / "reopen");
  benchmark_durable_expire(scratch / "expire");
  benchmark_verification(scratch / "verify");

  std::error_code error;
  std::filesystem::remove_all(scratch, error);
  check(!error, "the benchmark scratch directory is removed");
  check(!std::filesystem::exists(scratch), "no benchmark residue is left behind");

  if (g_failures != 0) {
    std::cerr << g_failures << " benchmark check(s) failed\n";
    return 1;
  }
  std::cout << "all benchmark checks passed; state was verified and residue removed\n";
  return 0;
}
