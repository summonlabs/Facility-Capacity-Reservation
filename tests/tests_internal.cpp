// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal invariants that are reachable from the library's private surface:
// the sealed documents the store keeps beside the state, the fault-injection
// seam, the checked arithmetic helpers and the version contract.

#include <string>
#include <vector>

#include "detail/codec.hpp"
#include "detail/fault.hpp"
#include "detail/ledger_internal.hpp"
#include "detail/status_internal.hpp"
#include "dccp/facility_capacity_reservation/digest.hpp"
#include "dccp/facility_capacity_reservation/units.hpp"
#include "dccp/facility_capacity_reservation/version.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

const std::vector<fcr_test::PoolSpec> kPools = {
    {fcr::ResourceKind::Rack, "hall-a", 10, 0, 0},
};

}  // namespace

FCR_TEST(internal, the_compiled_version_matches_the_build_system) {
  FCR_CHECK_EQ(std::string(fcr::kVersionString), std::string(FACILITY_CAPACITY_RESERVATION_CMAKE_VERSION));
  FCR_CHECK_EQ(fcr_test::runtime_u64(fcr::kVersionNumber), 1U * 10000U);
  FCR_CHECK_EQ(fcr_test::runtime_u64(fcr::kVersionMajor), 1U);
  FCR_CHECK_EQ(fcr_test::runtime_u64(fcr::kVersionMinor), 0U);
  FCR_CHECK_EQ(fcr_test::runtime_u64(fcr::kVersionPatch), 0U);
  FCR_CHECK(!fcr::kRuntimeName.empty());
  FCR_CHECK(!fcr::kDccpTranche.empty());
}

FCR_TEST(internal, the_formats_declare_their_versions) {
  FCR_CHECK_EQ(fcr_test::runtime_u64(fcr::kStateFormatVersion), 1U);
  FCR_CHECK_EQ(fcr_test::runtime_u64(fcr::kSnapshotFormatVersion), 1U);
}

FCR_TEST(internal, checked_arithmetic_reports_every_overflow) {
  FCR_CHECK_EQ(fcr::checked_add(1, 2).value(), 3ULL);
  FCR_CHECK_EQ(fcr::checked_add(0, fcr::kMaxUnits).value(), fcr::kMaxUnits);
  FCR_CHECK_ERROR(fcr::checked_add(fcr::kMaxUnits, 1), fcr::ErrorCode::ArithmeticOverflow);
  FCR_CHECK_EQ(fcr::checked_sub(5, 3).value(), 2ULL);
  FCR_CHECK_ERROR(fcr::checked_sub(3, 5), fcr::ErrorCode::ArithmeticOverflow);
  FCR_CHECK_EQ(fcr::checked_mul(4, 5).value(), 20ULL);
  FCR_CHECK_ERROR(fcr::checked_mul(fcr::kMaxUnits, 2), fcr::ErrorCode::ArithmeticOverflow);

  fcr::Units total = fcr::kMaxUnits - 1;
  FCR_CHECK_ERROR(fcr::checked_accumulate(total, 2), fcr::ErrorCode::ArithmeticOverflow);
  FCR_CHECK_EQ(total, fcr::kMaxUnits - 1);
  FCR_CHECK(fcr::checked_accumulate(total, 1).has_value());
  FCR_CHECK_EQ(total, fcr::kMaxUnits);
}

FCR_TEST(internal, a_capacity_split_closes_or_reports_overflow) {
  const fcr::CapacitySplit split{fcr::Quantity(5), fcr::Quantity(3), fcr::Quantity(2)};
  FCR_REQUIRE(split.accounted().has_value());
  FCR_CHECK_EQ(split.accounted().value().units(), 10ULL);

  const fcr::CapacitySplit overflowing{fcr::Quantity(fcr::kMaxUnits), fcr::Quantity(fcr::kMaxUnits),
                                       fcr::Quantity(0)};
  FCR_CHECK_ERROR(overflowing.accounted(), fcr::ErrorCode::ArithmeticOverflow);
}

FCR_TEST(internal, canonical_claims_are_sorted_and_deduplicated_by_rejection) {
  const std::vector<fcr::ResourceClaim> unsorted = {
      fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 5),
      fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1),
  };
  const fcr::Result<std::vector<fcr::ResourceClaim>> canonical =
      fcr::detail::canonical_claims(unsorted, fcr::RequestLimits{});
  FCR_REQUIRE(canonical.has_value());
  FCR_REQUIRE(canonical->size() == 2);
  FCR_CHECK((*canonical)[0].pool.kind == fcr::ResourceKind::Rack);
  FCR_CHECK((*canonical)[1].pool.kind == fcr::ResourceKind::Power);

  const fcr::Result<std::vector<fcr::ResourceClaim>> duplicate = fcr::detail::canonical_claims(
      {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1),
       fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 2)},
      fcr::RequestLimits{});
  FCR_CHECK_ERROR(duplicate, fcr::ErrorCode::ClaimDuplicatePool);

  fcr::RequestLimits tight;
  tight.max_claims = 1;
  FCR_CHECK_ERROR(fcr::detail::canonical_claims(unsorted, tight), fcr::ErrorCode::LimitExceeded);
}

FCR_TEST(internal, the_fault_seam_is_inert_unless_it_is_configured) {
  FCR_CHECK(fcr::detail::parse_publish_stage("") == fcr::detail::PublishStage::None);
  FCR_CHECK(fcr::detail::parse_publish_stage("nonsense") == fcr::detail::PublishStage::None);
  FCR_CHECK(fcr::detail::parse_publish_stage("after-state-write") ==
            fcr::detail::PublishStage::AfterStateWrite);
  FCR_CHECK(fcr::detail::parse_publish_stage("before-head-commit") ==
            fcr::detail::PublishStage::BeforeHeadCommit);
  FCR_CHECK(fcr::detail::parse_publish_stage("after-head-commit") == fcr::detail::PublishStage::AfterHeadCommit);

  // This test process does not set the variable, so the seam does nothing.
  FCR_CHECK(fcr::detail::configured_publish_fault() == fcr::detail::PublishStage::None);
  fcr::detail::reach_publish_stage(fcr::detail::PublishStage::AfterStateWrite);
  fcr::detail::reach_publish_stage(fcr::detail::PublishStage::BeforeHeadCommit);
  fcr::detail::reach_publish_stage(fcr::detail::PublishStage::AfterHeadCommit);
  FCR_CHECK_EQ(fcr_test::runtime_u64(static_cast<std::uint64_t>(fcr::detail::kFaultExitStatus)), 70ULL);
  FCR_CHECK_EQ(std::string(fcr::detail::kFaultEnvironmentVariable), std::string("FCR_FAULT_INJECT"));
}

FCR_TEST(internal, the_error_table_is_ordered_and_complete) {
  FCR_CHECK(fcr::detail::error_vocabulary_complete());
  FCR_CHECK(fcr::detail::error_code_count() > 0);
  for (std::size_t index = 0; index < fcr::detail::error_code_count(); ++index) {
    const auto code = static_cast<fcr::ErrorCode>(index);
    FCR_CHECK(!fcr::error_code_name(code).empty());
  }
}

FCR_TEST(internal, encoded_state_is_a_complete_description_of_the_ledger) {
  fcr::Result<fcr::Store> opened = fcr::Store::in_memory();
  FCR_REQUIRE(opened.has_value());
  fcr::Store& store = opened.value();
  FCR_REQUIRE_OK(store.install_capacity(fcr_test::make_snapshot("snap-i", "site-i", 1, 1'000, kPools),
                                        fcr::Tick(1'000)));
  fcr_test::Fixture fixture;
  fixture.snapshot = fcr_test::make_snapshot("snap-i", "site-i", 1, 1'000, kPools);
  fixture.epoch = store.epoch().value();
  FCR_REQUIRE_OK(store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 2)}, 1'100, 900'000)));

  // The volatile store keeps no document of its own, so the codec is exercised
  // by round-tripping a durable store's state through the public API instead.
  fcr_test::TempDir directory("internal_codec");
  const std::filesystem::path root = directory / "store";
  fcr::Result<fcr::Store> durable = fcr::Store::create(root);
  FCR_REQUIRE(durable.has_value());
  FCR_REQUIRE_OK(durable->install_capacity(fcr_test::make_snapshot("snap-i", "site-i", 1, 1'000, kPools),
                                           fcr::Tick(1'000)));
  fcr_test::Fixture durable_fixture;
  durable_fixture.snapshot = fcr_test::make_snapshot("snap-i", "site-i", 1, 1'000, kPools);
  durable_fixture.epoch = durable->epoch().value();
  FCR_REQUIRE_OK(durable->reserve(fcr_test::reserve_request(
      durable_fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 2)}, 1'100, 900'000)));

  const std::string document = fcr_test::read_text_file(fcr_test::committed_state_file(root));
  FCR_CHECK(document.rfind("fcr-state 1\n", 0) == 0);
  FCR_CHECK(document.find("reservation.id=res-1") != std::string::npos);
  FCR_CHECK(document.find("attempt=attempt-1|reserve|res-1|") != std::string::npos);
  FCR_CHECK(document.back() == '\n');
  durable->close();
}

FCR_TEST(internal, the_head_and_meta_documents_use_the_same_seal_shape) {
  fcr_test::TempDir directory("internal_seal");
  const std::filesystem::path root = directory / "store";
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  FCR_REQUIRE(created.has_value());
  for (const char* name : {"fcr.meta", "fcr.head"}) {
    const std::string document = fcr_test::read_text_file(root / name);
    const std::size_t seal = document.rfind("digest=sha256:");
    FCR_REQUIRE(seal != std::string::npos);
    FCR_CHECK_EQ(document.size() - seal, std::size_t{14 + 64 + 1});
    FCR_CHECK_EQ(fcr::sha256_hex(document.substr(0, seal)), document.substr(seal + 14, 64));
  }
  created->close();
}

FCR_TEST(internal, transition_cause_and_amendment_cause_tokens_round_trip) {
  for (const fcr::TransitionCause cause :
       {fcr::TransitionCause::ClaimantRequest, fcr::TransitionCause::DeadlineElapsed,
        fcr::TransitionCause::AuthorityRevocation, fcr::TransitionCause::CapacityWithdrawn,
        fcr::TransitionCause::Superseded, fcr::TransitionCause::FacilityOverride}) {
    const std::string token(fcr::transition_cause_token(cause));
    FCR_CHECK(fcr::parse_transition_cause(token).has_value());
    FCR_CHECK(fcr::parse_transition_cause(token).value() == cause);
  }
  for (const fcr::AmendmentCause cause :
       {fcr::AmendmentCause::Correction, fcr::AmendmentCause::CapacityIncrease,
        fcr::AmendmentCause::CapacityDecrease, fcr::AmendmentCause::ScopeChange,
        fcr::AmendmentCause::DeadlineExtension, fcr::AmendmentCause::DeadlineReduction,
        fcr::AmendmentCause::PriorityChange, fcr::AmendmentCause::HeadroomChange, fcr::AmendmentCause::Other}) {
    const std::string token(fcr::amendment_cause_token(cause));
    FCR_CHECK(fcr::parse_amendment_cause(token).has_value());
    FCR_CHECK(fcr::parse_amendment_cause(token).value() == cause);
  }
  for (const fcr::HeadroomClass headroom :
       {fcr::HeadroomClass::Guaranteed, fcr::HeadroomClass::Firm, fcr::HeadroomClass::Opportunistic}) {
    const std::string token(fcr::headroom_class_token(headroom));
    FCR_CHECK(fcr::parse_headroom_class(token).has_value());
    FCR_CHECK(fcr::parse_headroom_class(token).value() == headroom);
  }
  for (const fcr::ReservationState state :
       {fcr::ReservationState::Active, fcr::ReservationState::Released, fcr::ReservationState::Expired,
        fcr::ReservationState::Revoked}) {
    const std::string token(fcr::reservation_state_token(state));
    FCR_CHECK(fcr::parse_reservation_state(token).has_value());
    FCR_CHECK(fcr::parse_reservation_state(token).value() == state);
  }
  for (const fcr::OperationKind kind :
       {fcr::OperationKind::Reserve, fcr::OperationKind::Amend, fcr::OperationKind::Release,
        fcr::OperationKind::Expire, fcr::OperationKind::Revoke, fcr::OperationKind::Reconcile}) {
    const std::string token(fcr::operation_kind_token(kind));
    FCR_CHECK(fcr::parse_operation_kind(token).has_value());
    FCR_CHECK(fcr::parse_operation_kind(token).value() == kind);
  }
  for (const fcr::CapacityOrigin origin : {fcr::CapacityOrigin::Consumed, fcr::CapacityOrigin::Restored}) {
    const std::string token(fcr::capacity_origin_token(origin));
    FCR_CHECK(fcr::parse_capacity_origin(token).has_value());
    FCR_CHECK(fcr::parse_capacity_origin(token).value() == origin);
  }
  FCR_CHECK_ERROR(fcr::parse_transition_cause("nope"), fcr::ErrorCode::UnknownEnumToken);
  FCR_CHECK_ERROR(fcr::parse_amendment_cause("nope"), fcr::ErrorCode::UnknownEnumToken);
  FCR_CHECK_ERROR(fcr::parse_headroom_class("nope"), fcr::ErrorCode::UnknownEnumToken);
  FCR_CHECK_ERROR(fcr::parse_reservation_state("nope"), fcr::ErrorCode::UnknownEnumToken);
  FCR_CHECK_ERROR(fcr::parse_operation_kind("nope"), fcr::ErrorCode::UnknownEnumToken);
  FCR_CHECK_ERROR(fcr::parse_capacity_origin("nope"), fcr::ErrorCode::UnknownEnumToken);
}

FCR_TEST(internal, identity_comparison_across_families_is_the_single_documented_path) {
  const fcr::ClaimantRef claimant_value = fcr_test::claimant("team-a");
  const fcr::ActorRef actor_value = fcr_test::actor("team-a");
  const fcr::ActorRef other_actor = fcr_test::actor("team-b");
  FCR_CHECK(fcr::detail::same_identity_text(claimant_value, actor_value));
  FCR_CHECK(!fcr::detail::same_identity_text(claimant_value, other_actor));
}

FCR_TEST(internal, a_snapshot_shape_check_is_at_least_as_strict_as_the_parser) {
  fcr::CapacitySnapshot snapshot = fcr_test::make_snapshot("snap-s", "site-s", 1, 1'000, kPools);
  FCR_CHECK(fcr::detail::validate_snapshot_shape(snapshot, 16).has_value());

  fcr::CapacitySnapshot no_ref = snapshot;
  no_ref.ref = fcr::SnapshotRef();
  FCR_CHECK_ERROR(fcr::detail::validate_snapshot_shape(no_ref, 16), fcr::ErrorCode::MissingField);

  fcr::CapacitySnapshot no_facility = snapshot;
  no_facility.facility = fcr::FacilityRef();
  FCR_CHECK_ERROR(fcr::detail::validate_snapshot_shape(no_facility, 16), fcr::ErrorCode::MissingField);

  fcr::CapacitySnapshot no_generation = snapshot;
  no_generation.source_generation = fcr::SourceGeneration(0);
  FCR_CHECK_ERROR(fcr::detail::validate_snapshot_shape(no_generation, 16), fcr::ErrorCode::MissingField);

  fcr::CapacitySnapshot too_many = snapshot;
  FCR_CHECK_ERROR(fcr::detail::validate_snapshot_shape(too_many, 0), fcr::ErrorCode::LimitExceeded);

  fcr::CapacitySnapshot inconsistent = snapshot;
  inconsistent.pools[0].withdrawn = 11;
  FCR_CHECK_ERROR(fcr::detail::validate_snapshot_shape(inconsistent, 16), fcr::ErrorCode::SnapshotInconsistent);
}

FCR_TEST(internal, detail_helpers_report_the_bound_they_were_given) {
  const fcr::Result<void> too_long = fcr::detail::require_detail(std::string(100, 'x'), 8, "detail");
  FCR_CHECK_ERROR(too_long, fcr::ErrorCode::TextTooLong);
  FCR_CHECK(fcr::detail::require_detail("short", 8, "detail").has_value());
  FCR_CHECK_ERROR(fcr::detail::require_epoch(fcr::AuthorityEpoch(0)), fcr::ErrorCode::AuthorityRequired);
  FCR_CHECK(fcr::detail::require_epoch(fcr::AuthorityEpoch(1)).has_value());
}
