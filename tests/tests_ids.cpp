// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Strongly typed identities and counters: no cross-family conversion, checked
// exhaustion, and incarnation handling.

#include <string>
#include <type_traits>

#include "dccp/facility_capacity_reservation/strong_id.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

FCR_TEST(ids, no_identity_family_converts_to_another) {
  static_assert(!std::is_convertible_v<fcr::ClaimantRef, fcr::ActorRef>);
  static_assert(!std::is_convertible_v<fcr::TenantRef, fcr::ClaimantRef>);
  static_assert(!std::is_convertible_v<fcr::ReservationId, fcr::AttemptId>);
  static_assert(!std::is_convertible_v<fcr::ScopeRef, fcr::FacilityRef>);
  static_assert(!std::is_convertible_v<fcr::SnapshotRef, fcr::ServiceRef>);
  static_assert(!std::is_constructible_v<fcr::ActorRef, fcr::ClaimantRef>);
  static_assert(!std::is_convertible_v<std::string, fcr::ReservationId>);
  // The static assertions above are the test; this keeps the case observable in
  // the run log without asserting on a constant.
  FCR_CHECK(fcr_test::runtime_bool(fcr::ReservationId::parse("res-1", "id").has_value()));
}

FCR_TEST(ids, counters_do_not_convert_between_families) {
  static_assert(!std::is_convertible_v<fcr::ReservationGeneration, fcr::AuthorityEpoch>);
  static_assert(!std::is_convertible_v<fcr::Revision, fcr::SourceGeneration>);
  static_assert(!std::is_convertible_v<fcr::Tick, fcr::Revision>);
  static_assert(!std::is_convertible_v<std::uint64_t, fcr::Revision>);
  static_assert(std::is_constructible_v<fcr::Revision, std::uint64_t>);
  FCR_CHECK(fcr_test::runtime_bool(fcr::Revision(1).value() == 1ULL));
}

FCR_TEST(ids, parse_validates_and_reports_the_right_reason) {
  FCR_CHECK(fcr::ReservationId::parse("res-1", "reservation id").has_value());
  FCR_CHECK_ERROR(fcr::ReservationId::parse("", "reservation id"), fcr::ErrorCode::MissingField);
  FCR_CHECK_ERROR(fcr::ReservationId::parse(std::string(300, 'a'), "reservation id"),
                  fcr::ErrorCode::IdentifierTooLong);
  FCR_CHECK_ERROR(fcr::ReservationId::parse("has space", "reservation id"), fcr::ErrorCode::MalformedIdentifier);
  FCR_CHECK_ERROR(fcr::ReservationId::parse("../escape", "reservation id"), fcr::ErrorCode::MalformedIdentifier);
}

FCR_TEST(ids, empty_identity_renders_as_absent_in_documents) {
  const fcr::ActorRef absent;
  FCR_CHECK(absent.empty());
  FCR_CHECK_EQ(absent.to_string(), std::string("-"));
  FCR_CHECK_EQ(absent.value(), std::string(""));
}

FCR_TEST(ids, counters_compare_within_their_family) {
  const fcr::Revision low(1);
  const fcr::Revision high(2);
  FCR_CHECK(low < high);
  FCR_CHECK(high > low);
  FCR_CHECK(low <= low);
  FCR_CHECK(low >= low);
  FCR_CHECK(low != high);
  FCR_CHECK(fcr_test::runtime_bool(!low.is_zero()));
  FCR_CHECK(fcr_test::runtime_bool(fcr::Revision(0).is_zero()));
  FCR_CHECK_EQ(fcr_test::runtime_u64(fcr::ReservationGeneration::first().value()), 1ULL);
}

FCR_TEST(ids, counter_exhaustion_is_reported_not_wrapped) {
  const fcr::Revision maximum(fcr::Revision::kMax);
  FCR_CHECK_ERROR(maximum.next(), fcr::ErrorCode::ArithmeticOverflow);
  const fcr::Revision penultimate(fcr::Revision::kMax - 1);
  const fcr::Result<fcr::Revision> next = penultimate.next();
  FCR_REQUIRE(next.has_value());
  FCR_CHECK_EQ(next->value(), fcr::Revision::kMax);
}

FCR_TEST(ids, incarnations_are_unique_random_and_validated) {
  const fcr::Incarnation first = fcr::Incarnation::generate();
  const fcr::Incarnation second = fcr::Incarnation::generate();
  FCR_CHECK(!first.empty());
  FCR_CHECK_NE(first.to_string(), second.to_string());
  FCR_CHECK_EQ(first.to_string().size(), fcr::Incarnation::kHexDigits);
  FCR_CHECK(fcr::Incarnation::parse(first.to_string()).has_value());
  FCR_CHECK_EQ(fcr::Incarnation::parse(first.to_string()).value(), first);
}

FCR_TEST(ids, incarnation_parsing_refuses_everything_else) {
  FCR_CHECK_ERROR(fcr::Incarnation::parse(""), fcr::ErrorCode::MalformedIdentifier);
  FCR_CHECK_ERROR(fcr::Incarnation::parse("00"), fcr::ErrorCode::MalformedIdentifier);
  FCR_CHECK_ERROR(fcr::Incarnation::parse(std::string(32, 'A')), fcr::ErrorCode::MalformedIdentifier);
  FCR_CHECK_ERROR(fcr::Incarnation::parse(std::string(33, 'a')), fcr::ErrorCode::MalformedIdentifier);
  FCR_CHECK_ERROR(fcr::Incarnation::parse(std::string(31, 'a') + "z"), fcr::ErrorCode::MalformedIdentifier);
}

FCR_TEST(ids, identities_sort_byte_wise) {
  const fcr::ReservationId alpha = fcr::ReservationId::parse("alpha", "id").value();
  const fcr::ReservationId beta = fcr::ReservationId::parse("beta", "id").value();
  FCR_CHECK(alpha < beta);
  FCR_CHECK(!(beta < alpha));
  FCR_CHECK(alpha != beta);
}
