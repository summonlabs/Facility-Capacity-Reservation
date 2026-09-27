// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "child_process.hpp"
#include "dccp/facility_capacity_reservation/version.hpp"

namespace fcr_test {
namespace {

std::string g_current_context;
std::uint64_t g_seed = 0;
int g_failures = 0;
std::string g_filter;

/// Splits a comma-separated list into trimmed entries.
std::vector<std::string> split_list(const std::string& text) {
  std::vector<std::string> parts;
  std::string current;
  for (const char character : text) {
    if (character == ',') {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(character);
    }
  }
  parts.push_back(current);
  return parts;
}

/// True when `suite.case` matches the filter, which may name a suite alone.
bool matches_filter(const TestCase& test) {
  if (g_filter.empty()) {
    return true;
  }
  for (const std::string& entry : split_list(g_filter)) {
    if (entry.empty()) {
      continue;
    }
    if (test.suite == entry || test.name == entry || (test.suite + "." + test.name) == entry) {
      return true;
    }
  }
  return false;
}

std::uint64_t parse_seed(const std::string& text) {
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return 0;
    }
    value = value * 10U + static_cast<std::uint64_t>(character - '0');
  }
  return value;
}

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

int register_test(const char* suite, const char* name, void (*function)()) {
  registry().push_back(TestCase{suite, name, function});
  return 0;
}

void fail(const char* file, int line, const std::string& message) {
  ++g_failures;
  std::cout << "FAIL " << file << ":" << line << ": " << message << '\n';
  if (!g_current_context.empty()) {
    std::cout << "     context: " << g_current_context << '\n';
  }
  std::cout << "     seed: " << g_seed << '\n';
  std::cout.flush();
}

void note(const std::string& message) { std::cout << "     note: " << message << '\n'; }

void set_case_context(const std::string& context) { g_current_context = context; }

std::uint64_t current_seed() { return g_seed; }

std::string render(const std::string& value) { return value; }
std::string render(std::string_view value) { return std::string(value); }
std::string render(const char* value) { return std::string(value); }
std::string render(bool value) { return value ? "true" : "false"; }
std::string render(const std::error_code& value) { return value.message(); }

int run_all(int argc, char** argv) {
  const auto started = std::chrono::steady_clock::now();

  g_seed = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
  g_seed ^= static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

  bool list_only = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--list") {
      list_only = true;
    } else if (argument.rfind("--seed=", 0) == 0) {
      g_seed = parse_seed(argument.substr(7));
    } else if (argument.rfind("--filter=", 0) == 0) {
      g_filter = argument.substr(9);
    }
  }

  std::vector<TestCase> tests = registry();
  std::sort(tests.begin(), tests.end(), [](const TestCase& lhs, const TestCase& rhs) {
    if (lhs.suite != rhs.suite) {
      return lhs.suite < rhs.suite;
    }
    return lhs.name < rhs.name;
  });

  if (list_only) {
    for (const TestCase& test : tests) {
      std::cout << test.suite << '.' << test.name << '\n';
    }
    return 0;
  }

  std::cout << "facility_capacity_reservation tests " << dccp::facility_capacity_reservation::kVersionString
            << " (seed=" << g_seed << ")\n";

  int run = 0;
  int failed_tests = 0;
  for (const TestCase& test : tests) {
    if (!matches_filter(test)) {
      continue;
    }
    ++run;
    g_current_context = test.suite + "." + test.name;
    const int before = g_failures;
    std::cout << "RUN  " << test.suite << "." << test.name << '\n';
    std::cout.flush();
    try {
      test.function();
    } catch (const TestAborted&) {
      // The failure was already reported.
    } catch (const std::exception& error) {
      fail(__FILE__, __LINE__, std::string("unhandled exception: ") + error.what());
    } catch (...) {
      fail(__FILE__, __LINE__, "unhandled non-standard exception");
    }
    if (g_failures != before) {
      ++failed_tests;
      std::cout << "FAILED " << test.suite << "." << test.name << '\n';
    }
  }
  g_current_context.clear();

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
  std::cout << "ran " << run << " test(s), " << failed_tests << " failed, " << g_failures << " check failure(s), "
            << elapsed.count() << " ms\n";
  std::cout.flush();
  return failed_tests == 0 ? 0 : 1;
}

}  // namespace fcr_test

int main(int argc, char** argv) {
  // A child scenario re-executes this same binary; it must be dispatched before
  // anything else runs.
  int exit_code = 0;
  if (fcr_test::dispatch_child_scenario(argc, argv, exit_code)) {
    return exit_code;
  }
  return fcr_test::run_all(argc, argv);
}
