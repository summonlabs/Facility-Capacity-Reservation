// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The stable outcome vocabulary: completeness, distinctness, categorisation and
// the behaviour of the Error and Result value types.

#include <set>
#include <string>
#include <vector>

#include "detail/status_internal.hpp"
#include "dccp/facility_capacity_reservation/status.hpp"
#include "test_framework.hpp"

namespace fcr = dccp::facility_capacity_reservation;

FCR_TEST(status, vocabulary_is_complete_and_distinct) {
  FCR_CHECK(fcr::detail::error_vocabulary_complete());
  // A floor rather than an exact count: the vocabulary is append-only, so the
  // test documents that it is substantial without pinning it to today's size.
  FCR_CHECK(fcr::detail::error_code_count() >= 80);
}

FCR_TEST(status, every_declared_code_has_a_unique_name) {
  std::set<std::string> names;
  for (std::uint16_t value = 0; value <= static_cast<std::uint16_t>(fcr::ErrorCode::AttemptNotReplayable); ++value) {
    const auto code = static_cast<fcr::ErrorCode>(value);
    const std::string name(fcr::error_code_name(code));
    FCR_CHECK(!name.empty());
    FCR_CHECK_NE(name, std::string("UNRECOGNISED_ERROR_CODE"));
    FCR_CHECK(names.insert(name).second);
  }
}

FCR_TEST(status, categories_are_stable_for_representative_codes) {
  FCR_CHECK(fcr::error_category(fcr::ErrorCode::Ok) == fcr::ErrorCategory::Ok);
  FCR_CHECK(fcr::error_category(fcr::ErrorCode::MalformedIdentifier) == fcr::ErrorCategory::Argument);
  FCR_CHECK(fcr::error_category(fcr::ErrorCode::InsufficientCapacity) == fcr::ErrorCategory::Capacity);
  FCR_CHECK(fcr::error_category(fcr::ErrorCode::ReservationNotFound) == fcr::ErrorCategory::Reservation);
  FCR_CHECK(fcr::error_category(fcr::ErrorCode::StaleAuthorityEpoch) == fcr::ErrorCategory::Authority);
  FCR_CHECK(fcr::error_category(fcr::ErrorCode::StoreCorrupt) == fcr::ErrorCategory::Persistence);
  FCR_CHECK(fcr::error_category(fcr::ErrorCode::StoreClosed) == fcr::ErrorCategory::Lifecycle);
  FCR_CHECK(fcr::error_category(fcr::ErrorCode::LimitExceeded) == fcr::ErrorCategory::Limit);
  FCR_CHECK(fcr::error_category(fcr::ErrorCode::InvariantViolation) == fcr::ErrorCategory::Internal);
}

FCR_TEST(status, only_environmental_failures_are_retryable) {
  FCR_CHECK(fcr::error_is_retryable(fcr::ErrorCode::StoreLocked));
  FCR_CHECK(fcr::error_is_retryable(fcr::ErrorCode::IoError));
  FCR_CHECK(!fcr::error_is_retryable(fcr::ErrorCode::StaleAuthorityEpoch));
  FCR_CHECK(!fcr::error_is_retryable(fcr::ErrorCode::InsufficientCapacity));
  FCR_CHECK(!fcr::error_is_retryable(fcr::ErrorCode::Ok));
}

FCR_TEST(status, error_text_is_bounded) {
  const std::string huge(64 * 1024, 'x');
  const fcr::Error error(fcr::ErrorCode::InvalidArgument, huge);
  FCR_CHECK(error.message().size() <= 512);
  FCR_CHECK(error.to_string().size() <= 512 + 64);
  const fcr::Error subject = fcr::Error(fcr::ErrorCode::InvalidArgument, "short").with_subject(huge);
  FCR_CHECK(subject.subject().size() <= 256);
}

FCR_TEST(status, error_renders_code_message_and_subject) {
  const fcr::Error error = fcr::Error(fcr::ErrorCode::InsufficientCapacity, "not enough racks")
                               .with_subject("rack:hall-a");
  FCR_CHECK_EQ(error.to_string(), std::string("INSUFFICIENT_CAPACITY: not enough racks [subject=rack:hall-a]"));
  FCR_CHECK_EQ(std::string(fcr::error_category_name(error.category())), std::string("CAPACITY"));
}

FCR_TEST(status, a_result_built_without_a_value_or_error_is_an_internal_error) {
  const fcr::Result<int> broken(fcr::Error(fcr::ErrorCode::Ok, ""));
  FCR_CHECK(!broken.has_value());
  FCR_CHECK(broken.error().code() == fcr::ErrorCode::InternalError);
}

FCR_TEST(status, dereferencing_a_failed_result_throws_logic_error) {
  const fcr::Result<int> failed(fcr::Error(fcr::ErrorCode::InvalidArgument, "bad"));
  bool threw = false;
  try {
    (void)failed.value();
  } catch (const std::logic_error&) {
    threw = true;
  }
  FCR_CHECK(threw);
}

FCR_TEST(status, void_result_reports_success_by_having_a_value) {
  const fcr::Result<void> good = fcr::ok();
  FCR_CHECK(good.has_value());
  FCR_CHECK(good.error().ok());
  const fcr::Result<void> bad = fcr::Error(fcr::ErrorCode::IoError, "disk");
  FCR_CHECK(!bad.has_value());
  FCR_CHECK(bad.error().code() == fcr::ErrorCode::IoError);
}

FCR_TEST(status, result_moves_its_value) {
  fcr::Result<std::string> source(std::string("payload"));
  const std::string moved = std::move(source).value();
  FCR_CHECK_EQ(moved, std::string("payload"));
}
