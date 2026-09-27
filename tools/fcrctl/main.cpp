// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// fcrctl - inspection and administration tool for a Facility Capacity
// Reservation store.
//
// Every command prints a stable, line-oriented record of what it did: one
// "key=value" line per fact, then "result=ok" or "result=error" with a stable
// error code. Nothing is printed for a human to parse by eye that a script
// cannot parse by key.
//
// The tool is deliberately not an authority of its own: it opens the store,
// issues exactly one request through the library, prints the outcome and closes.
// It holds no state between invocations.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_capacity_reservation/capacity.hpp"
#include "dccp/facility_capacity_reservation/clock.hpp"
#include "dccp/facility_capacity_reservation/ledger.hpp"
#include "dccp/facility_capacity_reservation/request.hpp"
#include "dccp/facility_capacity_reservation/reservation.hpp"
#include "dccp/facility_capacity_reservation/status.hpp"
#include "dccp/facility_capacity_reservation/store.hpp"
#include "dccp/facility_capacity_reservation/text.hpp"
#include "dccp/facility_capacity_reservation/version.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

/// Exit status used by the child scenario the test suite re-executes.
constexpr std::string_view kUsage =
    "usage: fcrctl <command> [options]\n"
    "\n"
    "commands:\n"
    "  version\n"
    "  init                 --root DIR [--max-attempts N] [--max-reservations N]\n"
    "  status               --root DIR\n"
    "  list                 --root DIR\n"
    "  show                 --root DIR --id ID\n"
    "  pools                --root DIR\n"
    "  verify               --root DIR\n"
    "  revalidate           --root DIR [--now TICK]\n"
    "  install-capacity     --root DIR --snapshot FILE [--now TICK]\n"
    "  reserve              --root DIR --id ID --claimant C --tenant T --service S --priority P\n"
    "                       --claim KIND:SCOPE:AMOUNT... --start TICK --deadline TICK --attempt A\n"
    "                       [--headroom guaranteed|firm|opportunistic] [--source-generation N]\n"
    "                       [--revision N] [--now TICK]\n"
    "  amend                --root DIR --id ID --generation N --actor A --attempt A\n"
    "                       --claim KIND:SCOPE:AMOUNT... --start TICK --deadline TICK\n"
    "                       [--headroom CLASS] [--priority P] [--cause TOKEN] [--detail TEXT]\n"
    "                       [--source-generation N] [--revision N] [--now TICK]\n"
    "  release              --root DIR --id ID --generation N --actor A --attempt A\n"
    "                       [--detail TEXT] [--revision N] [--now TICK]\n"
    "  revoke               --root DIR --id ID --generation N --actor A --attempt A --policy P\n"
    "                       [--cause TOKEN] [--allow-guaranteed-override] [--detail TEXT]\n"
    "                       [--revision N] [--now TICK]\n"
    "  expire               --root DIR --attempt A [--revision N] [--now TICK]\n"
    "  reconcile            --root DIR --snapshot FILE --expected-source-generation N --actor A --attempt A\n"
    "                       [--mode observe|enforce] [--policy P] [--revision N] [--now TICK]\n"
    "  snapshot-write       --out FILE --ref R --facility F --source-generation N [--tick TICK]\n"
    "                       --pool KIND:SCOPE:GROSS:WITHDRAWN:FLOOR...\n"
    "  snapshot-show        --snapshot FILE\n"
    "\n"
    "common options: [--read-only] [--quiet] [--trust-persisted-capacity]\n"
    "\n"
    "capacity evidence:\n"
    "  A capacity-consuming command (reserve, amend) refuses capacity evidence that\n"
    "  was restored from persistence, because restored evidence is real capacity but\n"
    "  it is no longer known to be current. Consume a fresh snapshot with\n"
    "  install-capacity or reconcile, or pass --trust-persisted-capacity to state\n"
    "  explicitly that the restored evidence is to be treated as current.\n";

void print(std::string_view key, std::string_view value) {
  std::cout << key << '=' << value << '\n';
}

/// Exact-match overload: without it a string literal would convert to bool
/// through the standard pointer conversion and print "true".
void print(std::string_view key, const char* value) { print(key, std::string_view(value)); }

void print(std::string_view key, std::uint64_t value) { print(key, fcr::format_unsigned(value)); }

void print(std::string_view key, bool value) {
  print(key, value ? std::string_view("true") : std::string_view("false"));
}

int report_error(std::string_view command, const fcr::Error& error) {
  print("command", command);
  print("error-code", fcr::error_code_name(error.code()));
  print("error-category", fcr::error_category_name(error.category()));
  print("error-message", fcr::sanitize_for_display(error.message()));
  if (!error.subject().empty()) {
    print("error-subject", fcr::sanitize_for_display(error.subject()));
  }
  print("result", "error");
  std::cout.flush();
  return kExitFailure;
}

int report_usage(std::string_view message) {
  std::cerr << "fcrctl: " << message << "\n\n" << kUsage;
  return kExitUsage;
}

/// Renders untrusted text for a human-readable diagnostic.
std::string sanitize_for_output(std::string_view text) { return fcr::sanitize_for_display(text); }

}  // namespace

/// Binds the successful value of a Result, or reports the failure and returns
/// the tool's failure status.
#define FCR_TRY_RESULT(name, expression)                    \
  auto name##_result = (expression);                        \
  if (!name##_result.has_value()) {                         \
    return report_error(command, name##_result.error());    \
  }                                                         \
  auto& name = *name##_result

/// Reports the failure of a void Result and returns the tool's failure status.
#define FCR_TRY_RESULTV(expression)                         \
  do {                                                      \
    auto fcr_cli_result = (expression);                     \
    if (!fcr_cli_result.has_value()) {                      \
      return report_error(command, fcr_cli_result.error()); \
    }                                                       \
  } while (false)

namespace {

/// A parsed command line.
///
/// Options are "--key value" pairs or bare "--flag"s. A key that is immediately
/// followed by another key is recorded as a bare flag rather than left pending:
/// dropping it would silently change the meaning of the command.
class Arguments {
 public:
  Arguments(int argc, char** argv) {
    for (int index = 2; index < argc; ++index) {
      const std::string_view token(argv[index]);
      if (token.size() > 2 && token.substr(0, 2) == "--") {
        if (!pending_.empty()) {
          values_.emplace_back(pending_, std::string());
        }
        pending_ = std::string(token.substr(2));
      } else {
        values_.emplace_back(pending_, std::string(token));
        pending_.clear();
      }
    }
    if (!pending_.empty()) {
      values_.emplace_back(pending_, std::string());
      pending_.clear();
    }
  }

  /// True when the flag is present at all.
  bool has(std::string_view key) const {
    return std::any_of(values_.begin(), values_.end(), [key](const auto& entry) { return entry.first == key; });
  }

  std::optional<std::string> get(std::string_view key) const {
    for (const auto& entry : values_) {
      if (entry.first == key) {
        return entry.second;
      }
    }
    return std::nullopt;
  }

  std::string require(std::string_view key, bool& ok) const {
    const std::optional<std::string> value = get(key);
    if (!value.has_value() || value->empty()) {
      ok = false;
      return std::string();
    }
    return *value;
  }

  std::vector<std::string> all(std::string_view key) const {
    std::vector<std::string> found;
    for (const auto& entry : values_) {
      if (entry.first == key) {
        found.push_back(entry.second);
      }
    }
    return found;
  }

 private:
  std::vector<std::pair<std::string, std::string>> values_;
  std::string pending_;
};

fcr::Result<std::uint64_t> parse_tick(const std::string& text) {
  FCR_TRY(value, fcr::parse_unsigned(text));
  if (value == 0) {
    return fcr::Error(fcr::ErrorCode::InvalidArgument, "a tick must not be zero");
  }
  return value;
}

fcr::Result<std::vector<fcr::ResourceClaim>> parse_claims(const std::vector<std::string>& texts) {
  std::vector<fcr::ResourceClaim> claims;
  for (const std::string& text : texts) {
    FCR_TRY(fields, fcr::split_fields(text, ':', 4));
    if (fields.size() != 3) {
      return fcr::Error(fcr::ErrorCode::InvalidArgument,
                        "a claim must be written KIND:SCOPE:AMOUNT")
          .with_subject(fcr::sanitize_for_display(text));
    }
    FCR_TRY(kind, fcr::parse_resource_kind(fields[0]));
    FCR_TRY(scope, fcr::ScopeRef::parse(fields[1], "claim scope"));
    FCR_TRY(amount, fcr::parse_unsigned(fields[2]));
    fcr::ResourceClaim claim;
    claim.pool.kind = kind;
    claim.pool.scope = scope;
    claim.amount = amount;
    claims.push_back(std::move(claim));
  }
  return claims;
}

fcr::Result<fcr::CapacitySnapshot> parse_snapshot_file(const std::string& path) {
  std::error_code error;
  const std::filesystem::path file(path);
  if (!std::filesystem::is_regular_file(file, error) || error) {
    return fcr::Error(fcr::ErrorCode::SnapshotNotFound, "no capacity snapshot file exists at this path")
        .with_subject(path);
  }
  const std::uintmax_t size = std::filesystem::file_size(file, error);
  if (error) {
    return fcr::Error(fcr::ErrorCode::IoError, "cannot determine the snapshot file size").with_subject(path);
  }
  if (size > fcr::kMaxDocumentBytes) {
    return fcr::Error(fcr::ErrorCode::LimitExceeded, "the capacity snapshot exceeds the maximum document size")
        .with_subject(path);
  }
  std::string document(static_cast<std::size_t>(size), '\0');
  std::FILE* stream = nullptr;
#if defined(_WIN32)
  if (::_wfopen_s(&stream, file.wstring().c_str(), L"rb") != 0) {
    stream = nullptr;
  }
#else
  stream = std::fopen(path.c_str(), "rb");
#endif
  if (stream == nullptr) {
    return fcr::Error(fcr::ErrorCode::IoError, "cannot open the capacity snapshot file").with_subject(path);
  }
  const std::size_t read = document.empty() ? 0 : std::fread(document.data(), 1, document.size(), stream);
  const bool failed = std::ferror(stream) != 0;
  std::fclose(stream);
  if (failed || read != document.size()) {
    return fcr::Error(fcr::ErrorCode::IoError, "cannot read the capacity snapshot file").with_subject(path);
  }
  return fcr::parse_capacity_snapshot(document, fcr::SnapshotLimits{});
}

fcr::Result<void> write_file(const std::string& path, std::string_view content) {
  std::FILE* stream = nullptr;
#if defined(_WIN32)
  if (::_wfopen_s(&stream, std::filesystem::path(path).wstring().c_str(), L"wb") != 0) {
    stream = nullptr;
  }
#else
  stream = std::fopen(path.c_str(), "wb");
#endif
  if (stream == nullptr) {
    return fcr::Error(fcr::ErrorCode::IoError, "cannot open the output file for writing").with_subject(path);
  }
  const std::size_t written = content.empty() ? 0 : std::fwrite(content.data(), 1, content.size(), stream);
  const bool failed = std::ferror(stream) != 0;
  std::fclose(stream);
  if (failed || written != content.size()) {
    return fcr::Error(fcr::ErrorCode::IoError, "cannot write the output file").with_subject(path);
  }
  return fcr::ok();
}

fcr::StoreOptions store_options(const Arguments& arguments, bool& ok) {
  fcr::StoreOptions options;
  if (arguments.has("max-attempts")) {
    const std::string text = arguments.require("max-attempts", ok);
    if (!ok) {
      return options;
    }
    const fcr::Result<std::uint64_t> parsed = fcr::parse_unsigned(text);
    if (!parsed.has_value() || *parsed == 0) {
      ok = false;
      return options;
    }
    options.ledger.max_attempts = static_cast<std::size_t>(*parsed);
  }
  if (arguments.has("max-reservations")) {
    const std::string text = arguments.require("max-reservations", ok);
    if (!ok) {
      return options;
    }
    const fcr::Result<std::uint64_t> parsed = fcr::parse_unsigned(text);
    if (!parsed.has_value() || *parsed == 0) {
      ok = false;
      return options;
    }
    options.ledger.max_reservations = static_cast<std::size_t>(*parsed);
  }
  if (arguments.has("trust-persisted-capacity")) {
    options.trust_persisted_capacity = true;
  }
  return options;
}
void print_reservation(const fcr::ReservationView& view) {
  const fcr::ReservationRecord& record = view.record;
  print("reservation.id", record.id.value());
  print("reservation.generation", record.generation.value());
  print("reservation.authority-epoch", record.authority_epoch.value());
  print("reservation.revision", record.revision.value());
  print("reservation.last-attempt", record.last_attempt.value());
  print("reservation.claimant", record.claimant.value());
  print("reservation.tenant", record.tenant.value());
  print("reservation.service", record.service.value());
  print("reservation.priority", record.priority.value());
  print("reservation.headroom", fcr::headroom_class_token(record.headroom));
  print("reservation.source-snapshot", record.source_snapshot.to_string());
  print("reservation.source-generation", record.source_generation.value());
  print("reservation.start", record.validity.start.value());
  print("reservation.deadline", record.validity.deadline.value());
  print("reservation.state", fcr::reservation_state_token(record.state));
  print("reservation.created-at-tick", record.created_at_tick.value());
  print("reservation.updated-at-tick", record.updated_at_tick.value());
  print("reservation.source-stale", view.source_stale);
  print("reservation.deadline-passed", view.deadline_passed);
  print("reservation.lineage-length", static_cast<std::uint64_t>(record.lineage.size()));
  for (const fcr::ResourceClaim& claim : record.claims) {
    print("reservation.claim",
          claim.pool.to_string() + ":" + fcr::format_unsigned(claim.amount) + ":" +
              std::string(fcr::resource_kind_unit(claim.pool.kind)));
  }
  for (const fcr::AmendmentEntry& entry : record.lineage) {
    print("reservation.lineage",
          fcr::format_unsigned(entry.generation.value()) + ":" + fcr::format_unsigned(entry.predecessor.value()) +
              ":" + std::string(fcr::amendment_cause_token(entry.cause)) + ":" + entry.attempt.value());
  }
  if (record.termination.has_value()) {
    const fcr::TerminationProvenance& provenance = *record.termination;
    print("reservation.termination-cause", fcr::transition_cause_token(provenance.cause));
    print("reservation.termination-actor", provenance.actor.to_string());
    print("reservation.termination-attempt", provenance.attempt.value());
    print("reservation.termination-revision", provenance.revision.value());
    print("reservation.termination-epoch", provenance.epoch.value());
    print("reservation.termination-tick", provenance.at_tick.value());
    print("reservation.termination-policy", provenance.policy.has_value() ? provenance.policy->value() : "-");
    print("reservation.termination-detail", fcr::sanitize_for_display(provenance.detail));
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return report_usage("a command is required");
  }
  const std::string command(argv[1]);

  if (command == "--help" || command == "-h" || command == "help") {
    std::cout << kUsage;
    return kExitOk;
  }
  if (command == "version") {
    print("name", fcr::kRuntimeName);
    print("version", fcr::kVersionString);
    print("tranche", fcr::kDccpTranche);
    print("repository", fcr::kDccpRepositoryIndex);
    print("state-format-version", static_cast<std::uint64_t>(fcr::kStateFormatVersion));
    print("snapshot-format-version", static_cast<std::uint64_t>(fcr::kSnapshotFormatVersion));
    print("result", "ok");
    return kExitOk;
  }

  const Arguments arguments(argc, argv);
  bool ok = true;

  // The command name is validated before any option is interpreted, so an
  // unknown command is reported as such rather than as a missing option.
  static const char* const kCommands[] = {
      "version",     "init",             "status",    "list",         "show",
      "pools",       "verify",           "revalidate", "install-capacity", "reserve",
      "amend",       "release",          "revoke",    "expire",       "reconcile",
      "snapshot-write", "snapshot-show"};
  bool known = false;
  for (const char* candidate : kCommands) {
    if (command == candidate) {
      known = true;
      break;
    }
  }
  if (!known) {
    return report_usage("unknown command \"" + sanitize_for_output(command) + "\"");
  }

  // ---- snapshot-write ----------------------------------------------------
  if (command == "snapshot-write") {
    const std::string out = arguments.require("out", ok);
    const std::string ref = arguments.require("ref", ok);
    const std::string facility = arguments.require("facility", ok);
    const std::string generation_text = arguments.require("source-generation", ok);
    if (!ok) {
      return report_usage("snapshot-write requires --out, --ref, --facility and --source-generation");
    }
    fcr::CapacitySnapshot snapshot;
    FCR_TRY_RESULT(ref_id, fcr::SnapshotRef::parse(ref, "snapshot ref"));
    snapshot.ref = ref_id;
    FCR_TRY_RESULT(facility_id, fcr::FacilityRef::parse(facility, "facility ref"));
    snapshot.facility = facility_id;
    FCR_TRY_RESULT(generation, fcr::parse_unsigned(generation_text));
    if (generation == 0) {
      return report_error(command, fcr::Error(fcr::ErrorCode::InvalidArgument, "the source generation must not be zero"));
    }
    snapshot.source_generation = fcr::SourceGeneration(generation);
    snapshot.captured_at_tick = fcr::system_now_tick();
    if (arguments.has("tick")) {
      const std::string tick = arguments.require("tick", ok);
      if (!ok) {
        return report_usage("--tick requires a value");
      }
      FCR_TRY_RESULT(parsed_tick, parse_tick(tick));
      snapshot.captured_at_tick = fcr::Tick(parsed_tick);
    }
    for (const std::string& text : arguments.all("pool")) {
      FCR_TRY_RESULT(fields, fcr::split_fields(text, ':', 6));
      if (fields.size() != 5) {
        return report_error(command, fcr::Error(fcr::ErrorCode::InvalidArgument,
                                                "a pool must be written KIND:SCOPE:GROSS:WITHDRAWN:FLOOR"));
      }
      fcr::CapacityPool pool;
      FCR_TRY_RESULT(kind, fcr::parse_resource_kind(fields[0]));
      pool.key.kind = kind;
      FCR_TRY_RESULT(scope, fcr::ScopeRef::parse(fields[1], "pool scope"));
      pool.key.scope = scope;
      FCR_TRY_RESULT(gross, fcr::parse_unsigned(fields[2]));
      pool.gross = gross;
      FCR_TRY_RESULT(withdrawn, fcr::parse_unsigned(fields[3]));
      pool.withdrawn = withdrawn;
      FCR_TRY_RESULT(floor, fcr::parse_unsigned(fields[4]));
      pool.floor = floor;
      snapshot.pools.push_back(std::move(pool));
    }
    if (snapshot.pools.empty()) {
      return report_error(command, fcr::Error(fcr::ErrorCode::SnapshotEmpty, "at least one --pool is required"));
    }
    // The producer is responsible for canonical order; sorting here keeps the
    // tool usable without hiding the requirement from the library.
    std::sort(snapshot.pools.begin(), snapshot.pools.end(),
              [](const fcr::CapacityPool& lhs, const fcr::CapacityPool& rhs) { return lhs.key < rhs.key; });
    FCR_TRY_RESULT(document, fcr::write_capacity_snapshot(snapshot));
    FCR_TRY_RESULTV(write_file(out, document));
    print("command", command);
    print("snapshot.file", out);
    print("snapshot.ref", snapshot.ref.value());
    print("snapshot.source-generation", snapshot.source_generation.value());
    print("snapshot.pools", static_cast<std::uint64_t>(snapshot.pools.size()));
    print("result", "ok");
    return kExitOk;
  }

  // ---- snapshot-show -----------------------------------------------------
  if (command == "snapshot-show") {
    const std::string path = arguments.require("snapshot", ok);
    if (!ok) {
      return report_usage("snapshot-show requires --snapshot");
    }
    FCR_TRY_RESULT(snapshot, parse_snapshot_file(path));
    print("command", command);
    print("snapshot.ref", snapshot.ref.value());
    print("snapshot.source-generation", snapshot.source_generation.value());
    print("snapshot.facility", snapshot.facility.value());
    print("snapshot.captured-at-tick", snapshot.captured_at_tick.value());
    print("snapshot.body-digest", snapshot.body_digest);
    for (const fcr::CapacityPool& pool : snapshot.pools) {
      FCR_TRY_RESULT(available, pool.available());
      FCR_TRY_RESULT(reservable, pool.reservable());
      print("snapshot.pool", pool.key.to_string() + ":gross=" + fcr::format_unsigned(pool.gross) +
                                 ":withdrawn=" + fcr::format_unsigned(pool.withdrawn) +
                                 ":floor=" + fcr::format_unsigned(pool.floor) +
                                 ":available=" + fcr::format_unsigned(available) +
                                 ":reservable=" + fcr::format_unsigned(reservable));
    }
    print("result", "ok");
    return kExitOk;
  }

  const std::string root_text = arguments.require("root", ok);
  if (!ok) {
    return report_usage("this command requires --root");
  }
  const std::filesystem::path root(root_text);
  const bool quiet = arguments.has("quiet");
  const bool force_read_only = arguments.has("read-only");

  if (command == "init") {
    FCR_TRY_RESULT(store, fcr::Store::create(root, store_options(arguments, ok)));
    if (!ok) {
      return report_usage("invalid store option");
    }
    FCR_TRY_RESULT(epoch, store.epoch());
    FCR_TRY_RESULT(incarnation, store.incarnation());
    print("command", command);
    print("store.root", root.string());
    print("store.durable", store.durable());
    print("store.incarnation", incarnation.to_string());
    print("store.epoch", epoch.value());
    print("result", "ok");
    return kExitOk;
  }

  // Observation commands always take a shared lock; mutating commands take
  // write authority. Passing --read-only to a mutating command opens the store
  // read-only, and the mutation is then refused with STORE_READ_ONLY rather than
  // being silently upgraded.
  const auto open_store = [&](fcr::Result<fcr::Store>& slot) {
    const bool observation = command == "status" || command == "list" || command == "show" ||
                             command == "pools" || command == "verify" || command == "revalidate";
    if (observation || force_read_only) {
      slot = fcr::Store::open_read_only(root, store_options(arguments, ok));
    } else {
      slot = fcr::Store::open(root, store_options(arguments, ok));
    }
  };

  fcr::Result<fcr::Store> opened = fcr::Error(fcr::ErrorCode::InternalError, "unreachable");
  open_store(opened);
  if (!ok) {
    return report_usage("invalid store option");
  }
  if (!opened.has_value()) {
    return report_error(command, opened.error());
  }
  fcr::Store& store = opened.value();
  FCR_TRY_RESULT(epoch, store.epoch());
  FCR_TRY_RESULT(revision, store.revision());

  const auto now_tick = [&]() -> fcr::Result<fcr::Tick> {
    if (!arguments.has("now")) {
      return fcr::system_now_tick();
    }
    const std::string text = arguments.require("now", ok);
    if (!ok) {
      return fcr::Error(fcr::ErrorCode::InvalidArgument, "--now requires a value");
    }
    FCR_TRY(value, parse_tick(text));
    return fcr::Tick(value);
  };
  const auto revision_precondition = [&]() -> fcr::Result<fcr::Revision> {
    if (!arguments.has("revision")) {
      return revision;
    }
    const std::string text = arguments.require("revision", ok);
    if (!ok) {
      return fcr::Error(fcr::ErrorCode::InvalidArgument, "--revision requires a value");
    }
    FCR_TRY(value, fcr::parse_unsigned(text));
    return fcr::Revision(value);
  };

  if (command == "status") {
    FCR_TRY_RESULT(status, store.status());
    print("command", command);
    print("store.root", root.string());
    print("store.durable", status.durable);
    print("store.closed", status.closed);
    print("state.revision", status.revision.value());
    print("state.epoch", status.epoch.value());
    print("state.incarnation", status.incarnation.to_string());
    print("capacity.installed", status.pool_count != 0);
    print("capacity.fresh", status.capacity_fresh);
    print("capacity.source-snapshot", status.source_snapshot.to_string());
    print("capacity.source-generation", status.source_generation.value());
    print("capacity.facility", status.facility.to_string());
    print("capacity.pools", static_cast<std::uint64_t>(status.pool_count));
    print("reservations.total", static_cast<std::uint64_t>(status.reservation_count));
    print("reservations.active", static_cast<std::uint64_t>(status.active_count));
    print("attempts", static_cast<std::uint64_t>(status.attempt_count));
    print("result", "ok");
    return kExitOk;
  }

  if (command == "pools") {
    FCR_TRY_RESULT(accounts, store.pools());
    print("command", command);
    for (const fcr::PoolAccount& account : accounts) {
      const fcr::CapacitySplit split{fcr::Quantity(account.free), fcr::Quantity(account.committed),
                                     fcr::Quantity(account.protected_)};
      FCR_TRY_RESULT(accounted, split.accounted());
      print("pool", account.pool.to_string() + ":gross=" + fcr::format_unsigned(account.gross) +
                        ":withdrawn=" + fcr::format_unsigned(account.withdrawn) +
                        ":floor=" + fcr::format_unsigned(account.floor) +
                        ":available=" + fcr::format_unsigned(account.available) +
                        ":reservable=" + fcr::format_unsigned(account.reservable) +
                        ":committed=" + fcr::format_unsigned(account.committed) +
                        ":protected=" + fcr::format_unsigned(account.protected_) +
                        ":free=" + fcr::format_unsigned(account.free) +
                        ":accounted=" + fcr::format_unsigned(accounted.units()) +
                        ":active=" + fcr::format_unsigned(account.active_reservations));
    }
    print("pool.count", static_cast<std::uint64_t>(accounts.size()));
    print("result", "ok");
    return kExitOk;
  }

  if (command == "list") {
    FCR_TRY_RESULT(views, store.list());
    print("command", command);
    for (const fcr::ReservationView& view : views) {
      print("reservation", view.record.id.value() + ":generation=" +
                               fcr::format_unsigned(view.record.generation.value()) +
                               ":state=" + std::string(fcr::reservation_state_token(view.record.state)) +
                               ":headroom=" + std::string(fcr::headroom_class_token(view.record.headroom)) +
                               ":deadline=" + fcr::format_unsigned(view.record.validity.deadline.value()));
    }
    print("reservation.count", static_cast<std::uint64_t>(views.size()));
    print("result", "ok");
    return kExitOk;
  }

  if (command == "show") {
    const std::string id_text = arguments.require("id", ok);
    if (!ok) {
      return report_usage("show requires --id");
    }
    FCR_TRY_RESULT(id, fcr::ReservationId::parse(id_text, "reservation id"));
    FCR_TRY_RESULT(found, store.find(id));
    print("command", command);
    if (!found.has_value()) {
      FCR_TRY_RESULTV(fcr::Result<void>(
          fcr::Error(fcr::ErrorCode::ReservationNotFound, "no reservation with this identity is held")
              .with_subject(id.value())));
    }
    print_reservation(*found);
    print("result", "ok");
    return kExitOk;
  }

  if (command == "verify") {
    FCR_TRY_RESULT(report, store.verify());
    print("command", command);
    print("verify.ok", report.ok);
    print("verify.revision", report.revision.value());
    print("verify.epoch", report.epoch.value());
    print("verify.reservations", static_cast<std::uint64_t>(report.reservation_count));
    print("verify.active", static_cast<std::uint64_t>(report.active_count));
    print("verify.attempts", static_cast<std::uint64_t>(report.attempt_count));
    print("verify.pools", static_cast<std::uint64_t>(report.pool_count));
    print("verify.mismatched-pools", static_cast<std::uint64_t>(report.mismatched_pools.size()));
    for (const fcr::PoolKey& key : report.mismatched_pools) {
      print("verify.mismatched-pool", key.to_string());
    }
    print("result", report.ok ? "ok" : "error");
    return report.ok ? kExitOk : kExitFailure;
  }

  if (command == "revalidate") {
    FCR_TRY_RESULT(tick, now_tick());
    fcr::RevalidateRequest request;
    request.expected_revision = revision;
    request.now = tick;
    FCR_TRY_RESULT(report, store.revalidate(request));
    print("command", command);
    print("revalidate.revision", report.revision.value());
    print("revalidate.epoch", report.epoch.value());
    print("revalidate.now", report.now.value());
    print("revalidate.source-generation", report.source_generation.value());
    print("revalidate.current", static_cast<std::uint64_t>(report.current_count));
    print("revalidate.stale", static_cast<std::uint64_t>(report.stale_count));
    print("revalidate.deadline-passed", static_cast<std::uint64_t>(report.deadline_passed_count));
    print("revalidate.pool-missing", static_cast<std::uint64_t>(report.pool_missing_count));
    print("revalidate.capacity-exceeded", static_cast<std::uint64_t>(report.capacity_exceeded_count));
    for (const fcr::RevalidationEntry& entry : report.entries) {
      print("revalidate.entry", entry.id.value() + ":generation=" + fcr::format_unsigned(entry.generation.value()) +
                                    ":class=" + std::string(fcr::revalidation_class_token(entry.classification)) +
                                    ":detail=" + fcr::sanitize_for_display(entry.detail));
    }
    print("result", "ok");
    return kExitOk;
  }

  if (command == "install-capacity") {
    const std::string path = arguments.require("snapshot", ok);
    if (!ok) {
      return report_usage("install-capacity requires --snapshot");
    }
    FCR_TRY_RESULT(snapshot, parse_snapshot_file(path));
    FCR_TRY_RESULT(tick, now_tick());
    FCR_TRY_RESULTV(store.install_capacity(snapshot, tick));
    FCR_TRY_RESULT(new_revision, store.revision());
    print("command", command);
    print("capacity.source-snapshot", snapshot.ref.value());
    print("capacity.source-generation", snapshot.source_generation.value());
    print("capacity.pools", static_cast<std::uint64_t>(snapshot.pools.size()));
    print("state.revision", new_revision.value());
    print("result", "ok");
    return kExitOk;
  }

  const auto headroom_of = [&](const fcr::ReservationRecord* fallback) -> fcr::Result<fcr::HeadroomClass> {
    if (!arguments.has("headroom")) {
      if (fallback == nullptr) {
        return fcr::HeadroomClass::Guaranteed;
      }
      return fallback->headroom;
    }
    const std::string text = arguments.require("headroom", ok);
    if (!ok) {
      return fcr::Error(fcr::ErrorCode::InvalidArgument, "--headroom requires a value");
    }
    return fcr::parse_headroom_class(text);
  };

  if (command == "reserve") {
    const std::string id_text = arguments.require("id", ok);
    const std::string claimant_text = arguments.require("claimant", ok);
    const std::string tenant_text = arguments.require("tenant", ok);
    const std::string service_text = arguments.require("service", ok);
    const std::string priority_text = arguments.require("priority", ok);
    const std::string start_text = arguments.require("start", ok);
    const std::string deadline_text = arguments.require("deadline", ok);
    const std::string attempt_text = arguments.require("attempt", ok);
    if (!ok) {
      return report_usage(
          "reserve requires --id, --claimant, --tenant, --service, --priority, --start, --deadline and --attempt");
    }
    fcr::ReserveRequest request;
    FCR_TRY_RESULT(id, fcr::ReservationId::parse(id_text, "reservation id"));
    request.id = id;
    FCR_TRY_RESULT(claimant, fcr::ClaimantRef::parse(claimant_text, "claimant reference"));
    request.claimant = claimant;
    FCR_TRY_RESULT(tenant, fcr::TenantRef::parse(tenant_text, "tenant reference"));
    request.tenant = tenant;
    FCR_TRY_RESULT(service, fcr::ServiceRef::parse(service_text, "service reference"));
    request.service = service;
    FCR_TRY_RESULT(priority, fcr::PriorityRef::parse(priority_text, "priority reference"));
    request.priority = priority;
    FCR_TRY_RESULT(headroom, headroom_of(nullptr));
    request.headroom = headroom;
    FCR_TRY_RESULT(claims, parse_claims(arguments.all("claim")));
    request.claims = claims;
    FCR_TRY_RESULT(start, parse_tick(start_text));
    FCR_TRY_RESULT(deadline, parse_tick(deadline_text));
    request.validity.start = fcr::Tick(start);
    request.validity.deadline = fcr::Tick(deadline);
    FCR_TRY_RESULT(attempt, fcr::AttemptId::parse(attempt_text, "attempt id"));
    request.attempt = attempt;
    request.authority.epoch = epoch;
    FCR_TRY_RESULT(wanted_revision, revision_precondition());
    request.authority.revision = wanted_revision;
    if (arguments.has("source-generation")) {
      const std::string text = arguments.require("source-generation", ok);
      if (!ok) {
        return report_usage("--source-generation requires a value");
      }
      FCR_TRY_RESULT(generation, fcr::parse_unsigned(text));
      request.expected_source_generation = fcr::SourceGeneration(generation);
    } else {
      FCR_TRY_RESULT(status, store.status());
      request.expected_source_generation = status.source_generation;
    }
    FCR_TRY_RESULT(tick, now_tick());
    request.now = tick;

    FCR_TRY_RESULT(outcome, store.reserve(request));
    print("command", command);
    if (!quiet) {
      print_reservation(outcome.reservation);
    }
    print("outcome.replayed", outcome.replayed);
    print("state.revision", outcome.revision.value());
    print("state.epoch", outcome.epoch.value());
    print("result", "ok");
    return kExitOk;
  }

  if (command == "amend") {
    const std::string id_text = arguments.require("id", ok);
    const std::string generation_text = arguments.require("generation", ok);
    const std::string actor_text = arguments.require("actor", ok);
    const std::string attempt_text = arguments.require("attempt", ok);
    const std::string start_text = arguments.require("start", ok);
    const std::string deadline_text = arguments.require("deadline", ok);
    if (!ok) {
      return report_usage("amend requires --id, --generation, --actor, --attempt, --start and --deadline");
    }
    fcr::AmendRequest request;
    FCR_TRY_RESULT(id, fcr::ReservationId::parse(id_text, "reservation id"));
    request.id = id;
    FCR_TRY_RESULT(generation, fcr::parse_unsigned(generation_text));
    request.expected_generation = fcr::ReservationGeneration(generation);
    FCR_TRY_RESULT(actor, fcr::ActorRef::parse(actor_text, "actor reference"));
    request.actor = actor;
    FCR_TRY_RESULT(attempt, fcr::AttemptId::parse(attempt_text, "attempt id"));
    request.attempt = attempt;
    FCR_TRY_RESULT(claims, parse_claims(arguments.all("claim")));
    request.claims = claims;
    FCR_TRY_RESULT(start, parse_tick(start_text));
    FCR_TRY_RESULT(deadline, parse_tick(deadline_text));
    request.validity.start = fcr::Tick(start);
    request.validity.deadline = fcr::Tick(deadline);
    if (arguments.has("priority")) {
      const std::string text = arguments.require("priority", ok);
      if (!ok) {
        return report_usage("--priority requires a value");
      }
      FCR_TRY_RESULT(priority, fcr::PriorityRef::parse(text, "priority reference"));
      request.priority = priority;
    }
    if (arguments.has("headroom")) {
      FCR_TRY_RESULT(headroom, headroom_of(nullptr));
      request.headroom = headroom;
    }
    if (arguments.has("cause")) {
      const std::string text = arguments.require("cause", ok);
      if (!ok) {
        return report_usage("--cause requires a value");
      }
      FCR_TRY_RESULT(cause, fcr::parse_amendment_cause(text));
      request.cause = cause;
    }
    if (arguments.has("detail")) {
      request.detail = arguments.require("detail", ok);
      if (!ok) {
        return report_usage("--detail requires a value");
      }
    }
    request.authority.epoch = epoch;
    FCR_TRY_RESULT(wanted_revision, revision_precondition());
    request.authority.revision = wanted_revision;
    if (arguments.has("source-generation")) {
      const std::string text = arguments.require("source-generation", ok);
      if (!ok) {
        return report_usage("--source-generation requires a value");
      }
      FCR_TRY_RESULT(source, fcr::parse_unsigned(text));
      request.expected_source_generation = fcr::SourceGeneration(source);
    } else {
      FCR_TRY_RESULT(status, store.status());
      request.expected_source_generation = status.source_generation;
    }
    FCR_TRY_RESULT(tick, now_tick());
    request.now = tick;

    FCR_TRY_RESULT(outcome, store.amend(request));
    print("command", command);
    if (!quiet) {
      print_reservation(outcome.reservation);
    }
    print("outcome.previous-generation", outcome.previous_generation.value());
    print("outcome.replayed", outcome.replayed);
    print("state.revision", outcome.revision.value());
    print("result", "ok");
    return kExitOk;
  }

  if (command == "release") {
    const std::string id_text = arguments.require("id", ok);
    const std::string generation_text = arguments.require("generation", ok);
    const std::string actor_text = arguments.require("actor", ok);
    const std::string attempt_text = arguments.require("attempt", ok);
    if (!ok) {
      return report_usage("release requires --id, --generation, --actor and --attempt");
    }
    fcr::ReleaseRequest request;
    FCR_TRY_RESULT(id, fcr::ReservationId::parse(id_text, "reservation id"));
    request.id = id;
    FCR_TRY_RESULT(generation, fcr::parse_unsigned(generation_text));
    request.expected_generation = fcr::ReservationGeneration(generation);
    FCR_TRY_RESULT(actor, fcr::ActorRef::parse(actor_text, "actor reference"));
    request.actor = actor;
    FCR_TRY_RESULT(attempt, fcr::AttemptId::parse(attempt_text, "attempt id"));
    request.attempt = attempt;
    if (arguments.has("detail")) {
      request.detail = arguments.require("detail", ok);
      if (!ok) {
        return report_usage("--detail requires a value");
      }
    }
    request.authority.epoch = epoch;
    FCR_TRY_RESULT(wanted_revision, revision_precondition());
    request.authority.revision = wanted_revision;
    FCR_TRY_RESULT(tick, now_tick());
    request.now = tick;

    FCR_TRY_RESULT(outcome, store.release(request));
    print("command", command);
    if (!quiet) {
      print_reservation(outcome.reservation);
    }
    for (const fcr::ResourceClaim& claim : outcome.released) {
      print("outcome.released", claim.pool.to_string() + ":" + fcr::format_unsigned(claim.amount));
    }
    print("outcome.replayed", outcome.replayed);
    print("state.revision", outcome.revision.value());
    print("result", "ok");
    return kExitOk;
  }

  if (command == "revoke") {
    const std::string id_text = arguments.require("id", ok);
    const std::string generation_text = arguments.require("generation", ok);
    const std::string actor_text = arguments.require("actor", ok);
    const std::string attempt_text = arguments.require("attempt", ok);
    const std::string policy_text = arguments.require("policy", ok);
    if (!ok) {
      return report_usage("revoke requires --id, --generation, --actor, --attempt and --policy");
    }
    fcr::RevokeRequest request;
    FCR_TRY_RESULT(id, fcr::ReservationId::parse(id_text, "reservation id"));
    request.id = id;
    FCR_TRY_RESULT(generation, fcr::parse_unsigned(generation_text));
    request.expected_generation = fcr::ReservationGeneration(generation);
    FCR_TRY_RESULT(actor, fcr::ActorRef::parse(actor_text, "authority actor reference"));
    request.actor = actor;
    FCR_TRY_RESULT(attempt, fcr::AttemptId::parse(attempt_text, "attempt id"));
    request.attempt = attempt;
    FCR_TRY_RESULT(policy, fcr::PolicyRef::parse(policy_text, "policy reference"));
    request.policy = policy;
    request.allow_guaranteed_override = arguments.has("allow-guaranteed-override");
    if (arguments.has("cause")) {
      const std::string text = arguments.require("cause", ok);
      if (!ok) {
        return report_usage("--cause requires a value");
      }
      FCR_TRY_RESULT(cause, fcr::parse_transition_cause(text));
      request.cause = cause;
    }
    if (arguments.has("detail")) {
      request.detail = arguments.require("detail", ok);
      if (!ok) {
        return report_usage("--detail requires a value");
      }
    }
    request.authority.epoch = epoch;
    FCR_TRY_RESULT(wanted_revision, revision_precondition());
    request.authority.revision = wanted_revision;
    FCR_TRY_RESULT(tick, now_tick());
    request.now = tick;

    FCR_TRY_RESULT(outcome, store.revoke(request));
    print("command", command);
    if (!quiet) {
      print_reservation(outcome.reservation);
    }
    for (const fcr::ResourceClaim& claim : outcome.reclaimed) {
      print("outcome.reclaimed", claim.pool.to_string() + ":" + fcr::format_unsigned(claim.amount));
    }
    print("outcome.replayed", outcome.replayed);
    print("state.revision", outcome.revision.value());
    print("result", "ok");
    return kExitOk;
  }

  if (command == "expire") {
    const std::string attempt_text = arguments.require("attempt", ok);
    if (!ok) {
      return report_usage("expire requires --attempt");
    }
    fcr::ExpireRequest request;
    FCR_TRY_RESULT(attempt, fcr::AttemptId::parse(attempt_text, "attempt id"));
    request.attempt = attempt;
    request.authority.epoch = epoch;
    FCR_TRY_RESULT(wanted_revision, revision_precondition());
    request.authority.revision = wanted_revision;
    FCR_TRY_RESULT(tick, now_tick());
    request.now = tick;

    FCR_TRY_RESULT(outcome, store.expire(request));
    print("command", command);
    for (const fcr::ExpiredReservation& expired : outcome.expired) {
      print("outcome.expired", expired.id.value() + ":generation=" + fcr::format_unsigned(expired.generation.value()));
    }
    print("outcome.expired-count", static_cast<std::uint64_t>(outcome.expired.size()));
    print("outcome.replayed", outcome.replayed);
    print("state.revision", outcome.revision.value());
    print("result", "ok");
    return kExitOk;
  }

  if (command == "reconcile") {
    const std::string path = arguments.require("snapshot", ok);
    const std::string expected_text = arguments.require("expected-source-generation", ok);
    const std::string actor_text = arguments.require("actor", ok);
    const std::string attempt_text = arguments.require("attempt", ok);
    if (!ok) {
      return report_usage("reconcile requires --snapshot, --expected-source-generation, --actor and --attempt");
    }
    fcr::ReconcileRequest request;
    FCR_TRY_RESULT(snapshot, parse_snapshot_file(path));
    request.snapshot = snapshot;
    FCR_TRY_RESULT(expected, fcr::parse_unsigned(expected_text));
    request.expected_source_generation = fcr::SourceGeneration(expected);
    FCR_TRY_RESULT(actor, fcr::ActorRef::parse(actor_text, "reconciliation actor reference"));
    request.actor = actor;
    FCR_TRY_RESULT(attempt, fcr::AttemptId::parse(attempt_text, "attempt id"));
    request.attempt = attempt;
    if (arguments.has("mode")) {
      const std::string text = arguments.require("mode", ok);
      if (!ok) {
        return report_usage("--mode requires a value");
      }
      FCR_TRY_RESULT(mode, fcr::parse_reconcile_mode(text));
      request.mode = mode;
    }
    if (arguments.has("policy")) {
      const std::string text = arguments.require("policy", ok);
      if (!ok) {
        return report_usage("--policy requires a value");
      }
      FCR_TRY_RESULT(policy, fcr::PolicyRef::parse(text, "policy reference"));
      request.policy = policy;
    }
    request.authority.epoch = epoch;
    FCR_TRY_RESULT(wanted_revision, revision_precondition());
    request.authority.revision = wanted_revision;
    FCR_TRY_RESULT(tick, now_tick());
    request.now = tick;

    FCR_TRY_RESULT(outcome, store.reconcile(request));
    print("command", command);
    print("outcome.adopted", outcome.adopted);
    print("outcome.replayed", outcome.replayed);
    print("outcome.previous-source-generation", outcome.previous_source_generation.value());
    print("outcome.source-generation", outcome.source_generation.value());
    for (const fcr::PoolOvercommit& overcommit : outcome.overcommits) {
      print("outcome.overcommit", overcommit.pool.to_string() +
                                      ":reservable=" + fcr::format_unsigned(overcommit.reservable) +
                                      ":committed=" + fcr::format_unsigned(overcommit.committed) +
                                      ":protected=" + fcr::format_unsigned(overcommit.protected_) +
                                      ":excess=" + fcr::format_unsigned(overcommit.excess));
    }
    for (const fcr::FencedReservation& fenced : outcome.fenced) {
      print("outcome.fenced", fenced.id.value() + ":generation=" + fcr::format_unsigned(fenced.generation.value()) +
                                   ":headroom=" + std::string(fcr::headroom_class_token(fenced.headroom)));
    }
    print("outcome.fenced-count", static_cast<std::uint64_t>(outcome.fenced.size()));
    print("state.revision", outcome.revision.value());
    print("result", "ok");
    return kExitOk;
  }

  return report_usage("unknown command \"" + command + "\"");
}
