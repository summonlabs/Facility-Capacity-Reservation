// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Minimal deterministic test framework.
//
// There is no external dependency and no timing logic anywhere in it: every test
// runs to completion on its own. Randomized and property tests take an explicit
// seed so a failing case can be reproduced exactly, and the seed is printed with
// every failure.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_TESTS_TEST_FRAMEWORK_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_TESTS_TEST_FRAMEWORK_HPP

#include <cstdint>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "dccp/facility_capacity_reservation/status.hpp"

namespace fcr_test {

struct TestCase {
  std::string suite;
  std::string name;
  void (*function)();
};

std::vector<TestCase>& registry();
int register_test(const char* suite, const char* name, void (*function)());

/// Thrown by FCR_REQUIRE to abandon the remainder of a test body.
struct TestAborted {};

void fail(const char* file, int line, const std::string& message);
void note(const std::string& message);

/// Runs the whole suite. Returns the process exit status.
int run_all(int argc, char** argv);

/// Seed used by randomized tests in the current run.
std::uint64_t current_seed();

/// Records a reproducible marker printed with any failure in the current test.
void set_case_context(const std::string& context);

struct Registrar {
  Registrar(const char* suite, const char* name, void (*function)()) { register_test(suite, name, function); }
};

}  // namespace fcr_test

#define FCR_TEST(suite_name, case_name)                                                              \
  static void suite_name##_##case_name##_body();                                                     \
  static const ::fcr_test::Registrar suite_name##_##case_name##_registrar(                           \
      #suite_name, #case_name, &suite_name##_##case_name##_body);                                    \
  static void suite_name##_##case_name##_body()

#define FCR_FAIL(message) ::fcr_test::fail(__FILE__, __LINE__, (message))

#define FCR_CHECK(condition)                                        \
  do {                                                              \
    const bool fcr_test_ok = static_cast<bool>(condition);          \
    if (!fcr_test_ok) {                                             \
      FCR_FAIL(std::string("CHECK failed: ") + #condition);         \
    }                                                               \
  } while (false)

#define FCR_REQUIRE(condition)                                      \
  do {                                                              \
    const bool fcr_test_ok = static_cast<bool>(condition);          \
    if (!fcr_test_ok) {                                             \
      FCR_FAIL(std::string("REQUIRE failed: ") + #condition);       \
      throw ::fcr_test::TestAborted{};                              \
    }                                                               \
  } while (false)

#define FCR_CHECK_EQ(actual, expected)                                                                        \
  do {                                                                                                        \
    const auto fcr_test_actual = (actual);                                                                    \
    const auto fcr_test_expected = (expected);                                                                \
    if (!(fcr_test_actual == fcr_test_expected)) {                                                            \
      std::ostringstream fcr_test_stream;                                                                     \
      fcr_test_stream << "CHECK_EQ failed: " #actual " == " #expected " (actual="                             \
                      << ::fcr_test::render(fcr_test_actual) << ", expected="                                 \
                      << ::fcr_test::render(fcr_test_expected) << ")";                                        \
      FCR_FAIL(fcr_test_stream.str());                                                                        \
    }                                                                                                         \
  } while (false)

#define FCR_CHECK_NE(actual, unexpected)                                                                      \
  do {                                                                                                        \
    const auto fcr_test_actual = (actual);                                                                    \
    const auto fcr_test_unexpected = (unexpected);                                                            \
    if (fcr_test_actual == fcr_test_unexpected) {                                                             \
      std::ostringstream fcr_test_stream;                                                                     \
      fcr_test_stream << "CHECK_NE failed: " #actual " != " #unexpected " (actual="                           \
                      << ::fcr_test::render(fcr_test_actual) << ")";                                          \
      FCR_FAIL(fcr_test_stream.str());                                                                        \
    }                                                                                                         \
  } while (false)

/// Asserts that a Result failed with a specific stable error code.
#define FCR_CHECK_ERROR(expression, expected_code)                                                            \
  do {                                                                                                        \
    const auto& fcr_test_result = (expression);                                                               \
    if (fcr_test_result.has_value()) {                                                                        \
      FCR_FAIL(std::string("expected failure ") + #expected_code + " but the call succeeded: " #expression);   \
    } else if (fcr_test_result.error().code() != (expected_code)) {                                           \
      std::ostringstream fcr_test_stream;                                                                     \
      fcr_test_stream << "expected "                                                                          \
                      << ::dccp::facility_capacity_reservation::error_code_name(expected_code) << " but got "   \
                      << fcr_test_result.error().to_string();                                                 \
      FCR_FAIL(fcr_test_stream.str());                                                                        \
    }                                                                                                         \
  } while (false)

/// Asserts that a Result failed, without fixing the code.
#define FCR_CHECK_FAILED(expression)                                                                          \
  do {                                                                                                        \
    const auto& fcr_test_result = (expression);                                                               \
    if (fcr_test_result.has_value()) {                                                                        \
      FCR_FAIL(std::string("expected failure but the call succeeded: " #expression));                         \
    }                                                                                                         \
  } while (false)

/// Asserts that a Result succeeded, reporting the error and abandoning the test
/// body when it did not.
#define FCR_REQUIRE_OK(expression)                                                                            \
  do {                                                                                                        \
    const auto& fcr_test_result = (expression);                                                               \
    if (!fcr_test_result.has_value()) {                                                                       \
      FCR_FAIL(std::string("REQUIRE_OK failed: " #expression " -> ") + fcr_test_result.error().to_string());  \
      throw ::fcr_test::TestAborted{};                                                                        \
    }                                                                                                         \
  } while (false)

namespace fcr_test {

std::string render(const std::string& value);
std::string render(std::string_view value);
std::string render(const char* value);
std::string render(bool value);
std::string render(const std::error_code& value);

template <class T, class = void>
struct is_streamable : std::false_type {};

template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

/// Renders a value for a failure message: streamable values directly, iterable
/// values element by element, and anything else as a placeholder.
template <class T>
std::string render(const T& value) {
  if constexpr (is_streamable<T>::value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else if constexpr (requires { std::begin(value); std::end(value); }) {
    std::string out = "[";
    bool first = true;
    for (const auto& item : value) {
      if (!first) {
        out.append(", ");
      }
      first = false;
      out.append(render(item));
    }
    out.push_back(']');
    return out;
  } else {
    return "<value>";
  }
}

}  // namespace fcr_test

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_TESTS_TEST_FRAMEWORK_HPP
