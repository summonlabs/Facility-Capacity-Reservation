// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Independent-process helper for the multiprocess proof obligations.
//
// The test executable re-executes itself to obtain genuine operating-system
// processes: no threads pretending to be processes, no in-process simulation of
// a crash. A child scenario reports through its exit status and its captured
// output.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_TESTS_CHILD_PROCESS_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_TESTS_CHILD_PROCESS_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace fcr_test {

/// Absolute path of the running test executable.
std::filesystem::path executable_path();

struct ChildOutcome {
  /// Process exit status, or -1 when the process could not be started.
  int exit_code = -1;
  /// Combined standard output and standard error.
  std::string output;
};

/// Runs this executable as a new operating-system process.
///
/// Arguments are passed verbatim; the working directory is not changed, so
/// scenarios must use absolute paths.
ChildOutcome run_child(const std::vector<std::string>& arguments);

/// Runs an arbitrary program with its output captured.
ChildOutcome run_program(const std::filesystem::path& program, const std::vector<std::string>& arguments);

/// Starts this executable as a new process without waiting for it.
///
/// Used by the multiprocess race, which needs several writers alive at once.
class ChildHandle {
 public:
  ChildHandle() = default;
  ChildHandle(ChildHandle&&) noexcept;
  ChildHandle& operator=(ChildHandle&&) noexcept;
  ChildHandle(const ChildHandle&) = delete;
  ChildHandle& operator=(const ChildHandle&) = delete;
  ~ChildHandle();

  /// Waits for the process to finish and collects its output.
  ChildOutcome wait();

  bool valid() const noexcept;

 private:
  friend ChildHandle start_child(const std::filesystem::path& program, const std::vector<std::string>& arguments);
  struct Impl;
  explicit ChildHandle(Impl* impl) noexcept;

  Impl* impl_ = nullptr;
};

/// Starts a program without waiting for it.
ChildHandle start_child(const std::filesystem::path& program, const std::vector<std::string>& arguments);

/// Starts this executable as a new process without waiting for it.
ChildHandle start_child(const std::vector<std::string>& arguments);

/// Entry point implemented by the scenario translation unit.
int child_scenarios_main(int argc, char** argv);

/// When the process was started as a child scenario, runs it and stores its
/// exit status. Returns false for a normal test run.
bool dispatch_child_scenario(int argc, char** argv, int& exit_code);

/// True when `text` contains `needle`.
bool output_contains(const std::string& text, const std::string& needle);

/// Reads the value of a "key=value" line from captured output, or an empty
/// string when the key is absent.
std::string output_value(const std::string& text, const std::string& key);

}  // namespace fcr_test

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_TESTS_CHILD_PROCESS_HPP
