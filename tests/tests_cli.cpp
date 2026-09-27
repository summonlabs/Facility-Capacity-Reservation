// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The inspection tool, driven as an independent process through its real command
// line: the same path an operator or a script would take.

#include <algorithm>
#include <string>
#include <vector>

#include "child_process.hpp"
#include "dccp/facility_capacity_reservation/text.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

const std::filesystem::path kTool(FACILITY_CAPACITY_RESERVATION_CLI_PATH);

fcr_test::ChildOutcome run(const std::vector<std::string>& arguments) {
  std::vector<std::string> full;
  full.reserve(arguments.size());
  for (const std::string& argument : arguments) {
    full.push_back(argument);
  }
  return fcr_test::run_program(kTool, full);
}

std::string value_of(const fcr_test::ChildOutcome& outcome, const std::string& key) {
  return fcr_test::output_value(outcome.output, key);
}

}  // namespace

FCR_TEST(cli, version_reports_the_runtime_and_the_format_versions) {
  const fcr_test::ChildOutcome outcome = run({"version"});
  FCR_CHECK_EQ(outcome.exit_code, 0);
  FCR_CHECK_EQ(value_of(outcome, "name"), std::string("facility-capacity-reservation"));
  FCR_CHECK_EQ(value_of(outcome, "version"), std::string("1.0.0"));
  FCR_CHECK_EQ(value_of(outcome, "state-format-version"), std::string("1"));
  FCR_CHECK_EQ(value_of(outcome, "result"), std::string("ok"));
}

FCR_TEST(cli, help_lists_the_commands_and_exits_zero) {
  const fcr_test::ChildOutcome outcome = run({"--help"});
  FCR_CHECK_EQ(outcome.exit_code, 0);
  FCR_CHECK(fcr_test::output_contains(outcome.output, "reserve"));
  FCR_CHECK(fcr_test::output_contains(outcome.output, "reconcile"));
  FCR_CHECK(fcr_test::output_contains(outcome.output, "verify"));
}

FCR_TEST(cli, an_unknown_command_is_a_usage_error) {
  const fcr_test::ChildOutcome outcome = run({"nonsense"});
  FCR_CHECK_EQ(outcome.exit_code, 2);
  FCR_CHECK(fcr_test::output_contains(outcome.output, "unknown command"));
}

FCR_TEST(cli, a_missing_required_option_is_a_usage_error) {
  const fcr_test::ChildOutcome outcome = run({"status"});
  FCR_CHECK_EQ(outcome.exit_code, 2);
  FCR_CHECK(fcr_test::output_contains(outcome.output, "--root"));
}

FCR_TEST(cli, the_full_lifecycle_round_trips_through_the_tool) {
  fcr_test::TempDir directory("cli_lifecycle");
  const std::filesystem::path root = directory / "store";
  const std::filesystem::path snapshot = directory / "capacity.fcr";

  {
    const fcr_test::ChildOutcome outcome = run({"init", "--root", root.string()});
    FCR_CHECK_EQ(outcome.exit_code, 0);
    FCR_CHECK_EQ(value_of(outcome, "result"), std::string("ok"));
    FCR_CHECK_EQ(value_of(outcome, "store.durable"), std::string("true"));
  }
  {
    const fcr_test::ChildOutcome outcome =
        run({"snapshot-write", "--out", snapshot.string(), "--ref", "snap-cli", "--facility", "site-cli",
             "--source-generation", "1", "--tick", "1000", "--pool", "rack:hall-a:40:0:0",
             "--pool", "power:feed-a:400000:0:0"});
    FCR_CHECK_EQ(outcome.exit_code, 0);
    FCR_CHECK_EQ(value_of(outcome, "snapshot.pools"), std::string("2"));
  }
  {
    const fcr_test::ChildOutcome outcome = run({"snapshot-show", "--snapshot", snapshot.string()});
    FCR_CHECK_EQ(outcome.exit_code, 0);
    FCR_CHECK(fcr_test::output_contains(outcome.output, "reservable=40"));
    FCR_CHECK(fcr_test::output_contains(outcome.output, "reservable=400000"));
  }
  {
    const fcr_test::ChildOutcome outcome =
        run({"install-capacity", "--root", root.string(), "--snapshot", snapshot.string(), "--now", "1000"});
    FCR_CHECK_EQ(outcome.exit_code, 0);
    FCR_CHECK_EQ(value_of(outcome, "capacity.source-generation"), std::string("1"));
  }
  {
    const fcr_test::ChildOutcome outcome =
        run({"reserve", "--root", root.string(), "--id", "res-cli", "--claimant", "team-cli", "--tenant",
             "tenant-cli", "--service", "service-cli", "--priority", "priority-cli", "--attempt", "attempt-1",
             "--claim", "rack:hall-a:10", "--claim", "power:feed-a:50000", "--start", "1100", "--deadline",
             "900000", "--now", "1100", "--headroom", "firm", "--trust-persisted-capacity"});
    FCR_CHECK_EQ(outcome.exit_code, 0);
    FCR_CHECK_EQ(value_of(outcome, "reservation.id"), std::string("res-cli"));
    FCR_CHECK_EQ(value_of(outcome, "reservation.generation"), std::string("1"));
    FCR_CHECK_EQ(value_of(outcome, "reservation.headroom"), std::string("firm"));
  }
  {
    const fcr_test::ChildOutcome outcome = run({"pools", "--root", root.string()});
    FCR_CHECK_EQ(outcome.exit_code, 0);
    FCR_CHECK(fcr_test::output_contains(outcome.output, "rack:hall-a:gross=40"));
    FCR_CHECK(fcr_test::output_contains(outcome.output, "committed=10"));
    FCR_CHECK(fcr_test::output_contains(outcome.output, ":accounted=40"));
  }
  {
    const fcr_test::ChildOutcome outcome = run({"verify", "--root", root.string()});
    FCR_CHECK_EQ(outcome.exit_code, 0);
    FCR_CHECK_EQ(value_of(outcome, "verify.ok"), std::string("true"));
    FCR_CHECK_EQ(value_of(outcome, "verify.mismatched-pools"), std::string("0"));
  }
  {
    // A repeated attempt replays rather than reserving twice.
    const fcr_test::ChildOutcome outcome =
        run({"reserve", "--root", root.string(), "--id", "res-cli", "--claimant", "team-cli", "--tenant",
             "tenant-cli", "--service", "service-cli", "--priority", "priority-cli", "--attempt", "attempt-1",
             "--claim", "rack:hall-a:10", "--claim", "power:feed-a:50000", "--start", "1100", "--deadline",
             "900000", "--now", "1200", "--headroom", "firm", "--quiet", "--trust-persisted-capacity"});
    FCR_CHECK_EQ(outcome.exit_code, 0);
    FCR_CHECK_EQ(value_of(outcome, "outcome.replayed"), std::string("true"));
  }
  {
    const fcr_test::ChildOutcome outcome =
        run({"release", "--root", root.string(), "--id", "res-cli", "--generation", "1", "--actor", "team-cli",
             "--attempt", "attempt-2", "--now", "1300", "--quiet"});
    FCR_CHECK_EQ(outcome.exit_code, 0);
    FCR_CHECK_EQ(value_of(outcome, "outcome.replayed"), std::string("false"));
  }
  {
    const fcr_test::ChildOutcome outcome = run({"verify", "--root", root.string()});
    FCR_CHECK_EQ(outcome.exit_code, 0);
    FCR_CHECK_EQ(value_of(outcome, "verify.ok"), std::string("true"));
  }
  {
    const fcr_test::ChildOutcome outcome = run({"list", "--root", root.string()});
    FCR_CHECK_EQ(outcome.exit_code, 0);
    FCR_CHECK_EQ(value_of(outcome, "reservation.count"), std::string("1"));
    FCR_CHECK(fcr_test::output_contains(outcome.output, "state=released"));
  }
}

FCR_TEST(cli, a_rejected_request_prints_the_stable_code_and_exits_nonzero) {
  fcr_test::TempDir directory("cli_reject");
  const std::filesystem::path root = directory / "store";
  const std::filesystem::path snapshot = directory / "capacity.fcr";
  FCR_REQUIRE(run({"init", "--root", root.string()}).exit_code == 0);
  FCR_REQUIRE(run({"snapshot-write", "--out", snapshot.string(), "--ref", "snap-cli", "--facility", "site-cli",
                   "--source-generation", "1", "--tick", "1000", "--pool", "rack:hall-a:4:0:0"})
                  .exit_code == 0);
  FCR_REQUIRE(run({"install-capacity", "--root", root.string(), "--snapshot", snapshot.string(), "--now", "1000"})
                  .exit_code == 0);

  const fcr_test::ChildOutcome outcome =
      run({"reserve", "--root", root.string(), "--id", "res-big", "--claimant", "team-cli", "--tenant",
           "tenant-cli", "--service", "service-cli", "--priority", "priority-cli", "--attempt", "attempt-1",
           "--claim", "rack:hall-a:9", "--start", "1100", "--deadline", "900000", "--now", "1100",
               "--trust-persisted-capacity"});
  FCR_CHECK_EQ(outcome.exit_code, 1);
  FCR_CHECK_EQ(value_of(outcome, "error-code"), std::string("INSUFFICIENT_CAPACITY"));
  FCR_CHECK_EQ(value_of(outcome, "error-category"), std::string("CAPACITY"));
  FCR_CHECK_EQ(value_of(outcome, "result"), std::string("error"));
  FCR_CHECK(fcr_test::output_contains(outcome.output, "free=4"));
}

FCR_TEST(cli, a_malformed_snapshot_document_is_refused_with_a_stable_code) {
  fcr_test::TempDir directory("cli_bad_snapshot");
  const std::filesystem::path root = directory / "store";
  const std::filesystem::path snapshot = directory / "capacity.fcr";
  FCR_REQUIRE(run({"init", "--root", root.string()}).exit_code == 0);
  fcr_test::write_text_file(snapshot, "fcr-capacity-snapshot 1\nref=snap\n");
  const fcr_test::ChildOutcome outcome =
      run({"install-capacity", "--root", root.string(), "--snapshot", snapshot.string(), "--now", "1000"});
  FCR_CHECK_EQ(outcome.exit_code, 1);
  FCR_CHECK_EQ(value_of(outcome, "result"), std::string("error"));
  FCR_CHECK_NE(value_of(outcome, "error-code"), std::string(""));
}

FCR_TEST(cli, the_tool_refuses_an_absent_store_rather_than_creating_one) {
  fcr_test::TempDir directory("cli_absent");
  const fcr_test::ChildOutcome outcome = run({"status", "--root", (directory / "absent").string()});
  FCR_CHECK_EQ(outcome.exit_code, 1);
  FCR_CHECK_EQ(value_of(outcome, "error-code"), std::string("STORE_NOT_FOUND"));
}

FCR_TEST(cli, read_only_mode_refuses_a_mutation) {
  fcr_test::TempDir directory("cli_readonly");
  const std::filesystem::path root = directory / "store";
  FCR_REQUIRE(run({"init", "--root", root.string()}).exit_code == 0);
  const fcr_test::ChildOutcome outcome =
      run({"expire", "--root", root.string(), "--attempt", "attempt-1", "--now", "5000", "--read-only"});
  FCR_CHECK_EQ(outcome.exit_code, 1);
  FCR_CHECK_EQ(value_of(outcome, "error-code"), std::string("STORE_READ_ONLY"));
}

FCR_TEST(cli, a_store_held_by_another_process_is_reported_as_locked) {
  fcr_test::TempDir directory("cli_locked");
  const std::filesystem::path root = directory / "store";
  FCR_REQUIRE(run({"init", "--root", root.string()}).exit_code == 0);
  fcr::Result<fcr::Store> holder = fcr::Store::open(root);
  FCR_REQUIRE(holder.has_value());
  const fcr_test::ChildOutcome outcome = run({"list", "--root", root.string()});
  FCR_CHECK_EQ(outcome.exit_code, 1);
  FCR_CHECK_EQ(value_of(outcome, "error-code"), std::string("STORE_LOCKED"));
  holder->close();
}

FCR_TEST(cli, the_tool_never_writes_outside_the_store_it_was_given) {
  fcr_test::TempDir directory("cli_containment");
  const std::filesystem::path root = directory / "store";
  const std::filesystem::path outside = directory / "outside.txt";
  fcr_test::write_text_file(outside, "untouched");
  FCR_REQUIRE(run({"init", "--root", root.string()}).exit_code == 0);
  FCR_REQUIRE(run({"snapshot-write", "--out", (directory / "capacity.fcr").string(), "--ref", "snap-cli",
                   "--facility", "site-cli", "--source-generation", "1", "--tick", "1000", "--pool",
                   "rack:hall-a:4:0:0"})
                  .exit_code == 0);
  FCR_REQUIRE(run({"install-capacity", "--root", root.string(), "--snapshot",
                   (directory / "capacity.fcr").string(), "--now", "1000"})
                  .exit_code == 0);
  FCR_CHECK_EQ(fcr_test::read_text_file(outside), std::string("untouched"));
  FCR_CHECK(fcr_test::file_exists(root / "fcr.meta"));
}

FCR_TEST(cli, the_tool_handles_a_hostile_identifier_without_crashing) {
  fcr_test::TempDir directory("cli_hostile");
  const std::filesystem::path root = directory / "store";
  FCR_REQUIRE(run({"init", "--root", root.string()}).exit_code == 0);
  for (const char* hostile : {"../../escape", "a b", "id|pipe", "id=equals", ""}) {
    const fcr_test::ChildOutcome outcome =
        run({"show", "--root", root.string(), "--id", hostile});
    FCR_CHECK(outcome.exit_code == 1 || outcome.exit_code == 2);
    FCR_CHECK_NE(outcome.exit_code, 0);
  }
}

FCR_TEST(cli, reconcile_adopts_a_newer_snapshot_through_the_tool) {
  fcr_test::TempDir directory("cli_reconcile");
  const std::filesystem::path root = directory / "store";
  const std::filesystem::path first = directory / "capacity-1.fcr";
  const std::filesystem::path second = directory / "capacity-2.fcr";
  FCR_REQUIRE(run({"init", "--root", root.string()}).exit_code == 0);
  FCR_REQUIRE(run({"snapshot-write", "--out", first.string(), "--ref", "snap-cli", "--facility", "site-cli",
                   "--source-generation", "1", "--tick", "1000", "--pool", "rack:hall-a:40:0:0"})
                  .exit_code == 0);
  FCR_REQUIRE(run({"snapshot-write", "--out", second.string(), "--ref", "snap-cli", "--facility", "site-cli",
                   "--source-generation", "2", "--tick", "2000", "--pool", "rack:hall-a:10:0:0"})
                  .exit_code == 0);
  FCR_REQUIRE(run({"install-capacity", "--root", root.string(), "--snapshot", first.string(), "--now", "1000"})
                  .exit_code == 0);
  // A firm commitment, because reconciliation never fences a guaranteed one
  // automatically and an over-commit it cannot resolve changes nothing.
  FCR_REQUIRE(run({"reserve", "--root", root.string(), "--id", "res-cli", "--claimant", "team-cli", "--tenant",
                   "tenant-cli", "--service", "service-cli", "--priority", "priority-cli", "--attempt", "attempt-1",
                   "--claim", "rack:hall-a:30", "--start", "1100", "--deadline", "900000", "--now", "1100",
                   "--headroom", "firm", "--trust-persisted-capacity", "--quiet"})
                  .exit_code == 0);

  // Observation first: the reduced capacity would over-commit, and nothing may
  // change.
  {
    const fcr_test::ChildOutcome outcome =
        run({"reconcile", "--root", root.string(), "--snapshot", second.string(),
             "--expected-source-generation", "1", "--actor", "facility-authority", "--attempt", "observe-1",
             "--mode", "observe", "--now", "2000"});
    FCR_CHECK_EQ(outcome.exit_code, 0);
    FCR_CHECK_EQ(value_of(outcome, "outcome.adopted"), std::string("false"));
    FCR_CHECK(fcr_test::output_contains(outcome.output, "excess=20"));
  }
  {
    const fcr_test::ChildOutcome outcome = run({"status", "--root", root.string()});
    FCR_CHECK_EQ(value_of(outcome, "capacity.source-generation"), std::string("1"));
  }

  // Enforcing revokes the commitment that no longer fits and adopts the snapshot.
  {
    const fcr_test::ChildOutcome outcome =
        run({"reconcile", "--root", root.string(), "--snapshot", second.string(),
             "--expected-source-generation", "1", "--actor", "facility-authority", "--attempt", "enforce-1",
             "--mode", "enforce", "--policy", "policy-withdrawal", "--now", "2000"});
    FCR_CHECK_EQ(outcome.exit_code, 0);
    FCR_CHECK_EQ(value_of(outcome, "outcome.adopted"), std::string("true"));
    FCR_CHECK_EQ(value_of(outcome, "outcome.fenced-count"), std::string("1"));
    FCR_CHECK_EQ(value_of(outcome, "outcome.source-generation"), std::string("2"));
    FCR_CHECK(fcr_test::output_contains(outcome.output, "outcome.fenced=res-cli"));
  }
  {
    const fcr_test::ChildOutcome outcome = run({"verify", "--root", root.string()});
    FCR_CHECK_EQ(outcome.exit_code, 0);
    FCR_CHECK_EQ(value_of(outcome, "verify.ok"), std::string("true"));
  }
  {
    const fcr_test::ChildOutcome outcome = run({"pools", "--root", root.string()});
    FCR_CHECK_EQ(outcome.exit_code, 0);
    FCR_CHECK(fcr_test::output_contains(outcome.output, "reservable=10"));
    FCR_CHECK(fcr_test::output_contains(outcome.output, "committed=0"));
  }
}

FCR_TEST(cli, a_flag_followed_by_another_flag_is_not_dropped) {
  // The option parser must record a bare flag even when the next token is also a
  // flag; silently dropping one would change the meaning of the command.
  fcr_test::TempDir directory("cli_flags");
  const std::filesystem::path root = directory / "store";
  const std::filesystem::path snapshot = directory / "capacity.fcr";
  FCR_REQUIRE(run({"init", "--root", root.string()}).exit_code == 0);
  FCR_REQUIRE(run({"snapshot-write", "--out", snapshot.string(), "--ref", "snap-cli", "--facility", "site-cli",
                   "--source-generation", "1", "--tick", "1000", "--pool", "rack:hall-a:4:0:0"})
                  .exit_code == 0);
  FCR_REQUIRE(run({"install-capacity", "--root", root.string(), "--snapshot", snapshot.string(), "--now", "1000"})
                  .exit_code == 0);

  // Two flags in a row, in the order that used to lose the first one.
  const fcr_test::ChildOutcome outcome =
      run({"reserve", "--root", root.string(), "--id", "res-flags", "--claimant", "team-cli", "--tenant",
           "tenant-cli", "--service", "service-cli", "--priority", "priority-cli", "--attempt", "attempt-1",
           "--claim", "rack:hall-a:1", "--start", "1100", "--deadline", "900000", "--now", "1100",
           "--trust-persisted-capacity", "--quiet"});
  FCR_CHECK_EQ(outcome.exit_code, 0);

  // The other order works too.
  const fcr_test::ChildOutcome second =
      run({"reserve", "--root", root.string(), "--id", "res-flags-2", "--claimant", "team-cli", "--tenant",
           "tenant-cli", "--service", "service-cli", "--priority", "priority-cli", "--attempt", "attempt-2",
           "--claim", "rack:hall-a:1", "--start", "1100", "--deadline", "900000", "--now", "1100", "--quiet",
           "--trust-persisted-capacity"});
  FCR_CHECK_EQ(second.exit_code, 0);

  // Without the flag the restored capacity is refused, which is what proves the
  // flag was actually honoured rather than ignored.
  const fcr_test::ChildOutcome refused =
      run({"reserve", "--root", root.string(), "--id", "res-flags-3", "--claimant", "team-cli", "--tenant",
           "tenant-cli", "--service", "service-cli", "--priority", "priority-cli", "--attempt", "attempt-3",
           "--claim", "rack:hall-a:1", "--start", "1100", "--deadline", "900000", "--now", "1100"});
  FCR_CHECK_EQ(refused.exit_code, 1);
  FCR_CHECK_EQ(value_of(refused, "error-code"), std::string("CAPACITY_EVIDENCE_STALE"));
}

FCR_TEST(cli, show_reports_a_missing_reservation_with_the_stable_code) {
  fcr_test::TempDir directory("cli_show");
  const std::filesystem::path root = directory / "store";
  FCR_REQUIRE(run({"init", "--root", root.string()}).exit_code == 0);
  const fcr_test::ChildOutcome outcome = run({"show", "--root", root.string(), "--id", "res-absent"});
  FCR_CHECK_EQ(outcome.exit_code, 1);
  FCR_CHECK_EQ(value_of(outcome, "error-code"), std::string("RESERVATION_NOT_FOUND"));
}
