// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Child scenarios for the multiprocess proof obligations.
//
// This translation unit is compiled into the test executable, which re-executes
// itself with "--fcr-child <scenario>" to obtain a genuine operating-system
// process. A scenario reports through its exit status and through "key=value"
// lines on standard output.
//
// Every loop here is bounded by an explicit count. Nothing waits for another
// process to do something: a scenario either finishes its own fixed work or
// reports that it could not acquire the store.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "child_process.hpp"
#include "dccp/facility_capacity_reservation/capacity.hpp"
#include "dccp/facility_capacity_reservation/request.hpp"
#include "dccp/facility_capacity_reservation/status.hpp"
#include "dccp/facility_capacity_reservation/store.hpp"
#include "dccp/facility_capacity_reservation/text.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

constexpr int kChildOk = 0;
constexpr int kChildFailed = 1;
constexpr int kChildBlocked = 3;

void emit(const std::string& key, const std::string& value) { std::cout << key << '=' << value << '\n'; }

void emit_error(const fcr::Error& error) {
  emit("child.error-code", std::string(fcr::error_code_name(error.code())));
  emit("child.error-message", fcr::sanitize_for_display(error.message()));
}

/// Arguments after the scenario name, as "--key value" pairs.
class Options {
 public:
  Options(int argc, char** argv, int first) {
    for (int index = first; index + 1 < argc; index += 2) {
      values_.emplace_back(argv[index], argv[index + 1]);
    }
  }

  std::string get(const std::string& key, const std::string& fallback = std::string()) const {
    for (const auto& entry : values_) {
      if (entry.first == key) {
        return entry.second;
      }
    }
    return fallback;
  }

  std::uint64_t number(const std::string& key, std::uint64_t fallback = 0) const {
    const std::string text = get(key);
    if (text.empty()) {
      return fallback;
    }
    const fcr::Result<std::uint64_t> parsed = fcr::parse_unsigned(text);
    return parsed.has_value() ? *parsed : fallback;
  }

 private:
  std::vector<std::pair<std::string, std::string>> values_;
};

fcr::StoreOptions trusting_options() {
  fcr::StoreOptions options;
  // The race scenarios reopen the store repeatedly; the capacity evidence they
  // restore is the same snapshot the harness installed, and they opt in
  // explicitly rather than relying on it being treated as current by default.
  options.trust_persisted_capacity = true;
  return options;
}

fcr::ReserveRequest make_request(const fcr::Store& store, const std::string& id, const std::string& attempt_name,
                                 std::uint64_t amount) {
  fcr::ReserveRequest request;
  request.attempt = fcr::AttemptId::parse(attempt_name, "attempt").value();
  request.id = fcr::ReservationId::parse(id, "reservation id").value();
  request.claimant = fcr::ClaimantRef::parse("child-claimant", "claimant").value();
  request.tenant = fcr::TenantRef::parse("child-tenant", "tenant").value();
  request.service = fcr::ServiceRef::parse("child-service", "service").value();
  request.priority = fcr::PriorityRef::parse("child-priority", "priority").value();
  request.headroom = fcr::HeadroomClass::Guaranteed;
  request.authority.epoch = store.epoch().value();
  request.expected_source_generation = store.status().value().source_generation;
  request.validity.start = fcr::Tick(2'000);
  request.validity.deadline = fcr::Tick(900'000);
  request.now = fcr::Tick(2'000);
  request.claims = {fcr::ResourceClaim{{fcr::ResourceKind::Rack, fcr::ScopeRef::parse("hall-a", "scope").value()},
                                       amount}};
  return request;
}

/// Sets an environment variable before the library reads it.
bool set_environment(const std::string& name, const std::string& value) {
#if defined(_WIN32)
  return ::_putenv_s(name.c_str(), value.c_str()) == 0;
#else
  return ::setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
}

int scenario_open_store(const Options& options) {
  const std::filesystem::path root(options.get("--root"));
  fcr::Result<fcr::Store> opened = fcr::Store::open(root);
  if (!opened.has_value()) {
    if (opened.error().code() == fcr::ErrorCode::StoreLocked) {
      emit("child.open", "blocked");
      emit_error(opened.error());
      return kChildBlocked;
    }
    emit("child.open", "failed");
    emit_error(opened.error());
    return kChildFailed;
  }
  emit("child.open", "ok");
  emit("child.epoch", fcr::format_unsigned(opened->epoch().value().value()));
  opened->close();
  return kChildOk;
}

int scenario_die_holding(const Options& options) {
  const std::filesystem::path root(options.get("--root"));
  const std::string id = options.get("--id");
  fcr::Result<fcr::Store> opened = fcr::Store::open(root, trusting_options());
  if (!opened.has_value()) {
    emit_error(opened.error());
    return kChildFailed;
  }
  emit("child.incarnation", opened->incarnation().value().to_string());
  emit("child.epoch", fcr::format_unsigned(opened->epoch().value().value()));

  const fcr::Result<fcr::ReserveOutcome> outcome =
      opened->reserve(make_request(*opened, id, "attempt-" + id, 5));
  if (!outcome.has_value()) {
    emit_error(outcome.error());
    return kChildFailed;
  }
  emit("child.committed", "true");
  emit("child.revision", fcr::format_unsigned(outcome->revision.value()));
  std::cout.flush();
  // Terminate without unwinding: no destructor runs, so the store is never
  // closed by this process's own code and the lock is released only because the
  // operating system releases it.
  std::_Exit(kChildOk);
}

int scenario_crash_publishing(const Options& options) {
  const std::filesystem::path root(options.get("--root"));
  const std::string id = options.get("--id");
  const std::string stage = options.get("--fault");
  if (!set_environment("FCR_FAULT_INJECT", stage)) {
    emit("child.fault", "unsettable");
    return kChildFailed;
  }
  fcr::Result<fcr::Store> opened = fcr::Store::open(root, trusting_options());
  if (!opened.has_value()) {
    emit_error(opened.error());
    return kChildFailed;
  }
  // The publication path terminates this process at the configured stage.
  const fcr::Result<fcr::ReserveOutcome> outcome = opened->reserve(make_request(*opened, id, "attempt-" + id, 4));
  // Reaching this point means the fault did not fire.
  emit("child.fault", outcome.has_value() ? "not-fired" : "rejected");
  return kChildFailed;
}

int scenario_race_writer(const Options& options) {
  const std::filesystem::path root(options.get("--root"));
  const std::string writer = options.get("--writer");
  const std::uint64_t rounds = options.number("--rounds", 25);
  std::uint64_t accepted = 0;
  std::uint64_t blocked = 0;
  for (std::uint64_t round = 0; round < rounds; ++round) {
    fcr::Result<fcr::Store> opened = fcr::Store::open(root, trusting_options());
    if (!opened.has_value()) {
      if (opened.error().code() == fcr::ErrorCode::StoreLocked) {
        ++blocked;
        continue;
      }
      emit_error(opened.error());
      return kChildFailed;
    }
    const std::string id = "res-" + writer + "-" + fcr::format_unsigned(round);
    const fcr::Result<fcr::ReserveOutcome> outcome =
        opened->reserve(make_request(*opened, id, "attempt-" + id, 1));
    if (outcome.has_value()) {
      ++accepted;
    }
    opened->close();
  }
  emit("child.accepted", fcr::format_unsigned(accepted));
  emit("child.blocked", fcr::format_unsigned(blocked));
  return kChildOk;
}

int scenario_read_store(const Options& options) {
  const std::filesystem::path root(options.get("--root"));
  fcr::Result<fcr::Store> opened = fcr::Store::open_read_only(root);
  if (!opened.has_value()) {
    if (opened.error().code() == fcr::ErrorCode::StoreLocked) {
      emit("child.open", "blocked");
      emit_error(opened.error());
      return kChildBlocked;
    }
    emit_error(opened.error());
    return kChildFailed;
  }
  const fcr::Result<std::vector<fcr::ReservationView>> views = opened->list();
  if (!views.has_value()) {
    emit_error(views.error());
    return kChildFailed;
  }
  emit("child.reservations", fcr::format_unsigned(static_cast<std::uint64_t>(views->size())));
  const fcr::Result<fcr::VerificationReport> report = opened->verify();
  emit("child.closure", (report.has_value() && report->ok) ? "ok" : "broken");
  const fcr::Result<fcr::LedgerStatus> status = opened->status();
  emit("child.capacity-fresh",
       (status.has_value() && status->capacity_fresh) ? std::string("true") : std::string("false"));
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = opened->pools();
  if (!pools.has_value()) {
    return kChildFailed;
  }
  for (const fcr::PoolAccount& account : *pools) {
    emit("child.pool", account.pool.to_string() + ":committed=" + fcr::format_unsigned(account.committed) +
                           ":free=" + fcr::format_unsigned(account.free) +
                           ":reservable=" + fcr::format_unsigned(account.reservable));
  }
  opened->close();
  return kChildOk;
}

int scenario_use_stale_epoch(const Options& options) {
  const std::filesystem::path root(options.get("--root"));
  const std::uint64_t epoch = options.number("--epoch");
  fcr::Result<fcr::Store> opened = fcr::Store::open(root, trusting_options());
  if (!opened.has_value()) {
    emit_error(opened.error());
    return kChildFailed;
  }
  fcr::ReserveRequest request = make_request(*opened, "res-stale", "attempt-stale", 1);
  request.authority.epoch = fcr::AuthorityEpoch(epoch);
  const fcr::Result<fcr::ReserveOutcome> outcome = opened->reserve(request);
  if (outcome.has_value()) {
    emit("child.error-code", "NONE");
    opened->close();
    return kChildFailed;
  }
  emit("child.error-code", std::string(fcr::error_code_name(outcome.error().code())));
  opened->close();
  return outcome.error().code() == fcr::ErrorCode::StaleAuthorityEpoch ? kChildOk : kChildFailed;
}

int scenario_count_opens(const Options& options) {
  const std::filesystem::path root(options.get("--root"));
  const std::uint64_t rounds = options.number("--rounds", 20);
  std::uint64_t opens = 0;
  for (std::uint64_t round = 0; round < rounds; ++round) {
    fcr::Result<fcr::Store> opened = fcr::Store::open(root, trusting_options());
    if (!opened.has_value()) {
      if (opened.error().code() == fcr::ErrorCode::StoreLocked) {
        continue;
      }
      emit_error(opened.error());
      return kChildFailed;
    }
    ++opens;
    opened->close();
  }
  emit("child.opens", fcr::format_unsigned(opens));
  return kChildOk;
}

}  // namespace

int fcr_test::child_scenarios_main(int argc, char** argv) {
  if (argc < 3) {
    std::cout << "child.missing-scenario=1\n";
    return kChildFailed;
  }
  const std::string scenario(argv[2]);
  const Options options(argc, argv, 3);

  if (scenario == "open-store") {
    return scenario_open_store(options);
  }
  if (scenario == "die-holding") {
    return scenario_die_holding(options);
  }
  if (scenario == "crash-publishing") {
    return scenario_crash_publishing(options);
  }
  if (scenario == "race-writer") {
    return scenario_race_writer(options);
  }
  if (scenario == "read-store") {
    return scenario_read_store(options);
  }
  if (scenario == "use-stale-epoch") {
    return scenario_use_stale_epoch(options);
  }
  if (scenario == "count-opens") {
    return scenario_count_opens(options);
  }
  std::cout << "child.unknown-scenario=" << scenario << '\n';
  return kChildFailed;
}
