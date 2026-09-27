// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity_reservation/status.hpp"

#include "detail/status_internal.hpp"

#include <algorithm>

namespace dccp::facility_capacity_reservation {
namespace {

using Codes = std::pair<ErrorCode, std::string_view>;

/// The stable textual vocabulary. Every enumerator appears exactly once; a test
/// asserts that the table is complete, sorted and free of duplicates.
constexpr Codes kCodeNames[] = {
    {ErrorCode::Ok, "OK"},
    {ErrorCode::InvalidArgument, "INVALID_ARGUMENT"},
    {ErrorCode::MalformedIdentifier, "MALFORMED_IDENTIFIER"},
    {ErrorCode::IdentifierTooLong, "IDENTIFIER_TOO_LONG"},
    {ErrorCode::TextTooLong, "TEXT_TOO_LONG"},
    {ErrorCode::UnknownEnumToken, "UNKNOWN_ENUM_TOKEN"},
    {ErrorCode::MissingField, "MISSING_FIELD"},
    {ErrorCode::DuplicateField, "DUPLICATE_FIELD"},
    {ErrorCode::MalformedRecord, "MALFORMED_RECORD"},
    {ErrorCode::UnsupportedFormatVersion, "UNSUPPORTED_FORMAT_VERSION"},
    {ErrorCode::CountMismatch, "COUNT_MISMATCH"},
    {ErrorCode::TruncatedInput, "TRUNCATED_INPUT"},
    {ErrorCode::DigestMismatch, "DIGEST_MISMATCH"},
    {ErrorCode::FieldNotAllowedHere, "FIELD_NOT_ALLOWED_HERE"},
    {ErrorCode::NonCanonicalOrder, "NON_CANONICAL_ORDER"},
    {ErrorCode::LimitExceeded, "LIMIT_EXCEEDED"},
    {ErrorCode::ArithmeticOverflow, "ARITHMETIC_OVERFLOW"},
    {ErrorCode::SnapshotNotFound, "SNAPSHOT_NOT_FOUND"},
    {ErrorCode::SnapshotMalformed, "SNAPSHOT_MALFORMED"},
    {ErrorCode::SnapshotInconsistent, "SNAPSHOT_INCONSISTENT"},
    {ErrorCode::SnapshotEmpty, "SNAPSHOT_EMPTY"},
    {ErrorCode::PoolUnknown, "POOL_UNKNOWN"},
    {ErrorCode::PoolKindMismatch, "POOL_KIND_MISMATCH"},
    {ErrorCode::CapacityEvidenceStale, "CAPACITY_EVIDENCE_STALE"},
    {ErrorCode::SourceGenerationStale, "SOURCE_GENERATION_STALE"},
    {ErrorCode::SourceGenerationConflict, "SOURCE_GENERATION_CONFLICT"},
    {ErrorCode::ReservationNotFound, "RESERVATION_NOT_FOUND"},
    {ErrorCode::ReservationAlreadyExists, "RESERVATION_ALREADY_EXISTS"},
    {ErrorCode::ReservationNotActive, "RESERVATION_NOT_ACTIVE"},
    {ErrorCode::ReservationTerminal, "RESERVATION_TERMINAL"},
    {ErrorCode::ClaimEmpty, "CLAIM_EMPTY"},
    {ErrorCode::ClaimAmountZero, "CLAIM_AMOUNT_ZERO"},
    {ErrorCode::ClaimDuplicatePool, "CLAIM_DUPLICATE_POOL"},
    {ErrorCode::ClaimSetUnsorted, "CLAIM_SET_UNSORTED"},
    {ErrorCode::InvalidInterval, "INVALID_INTERVAL"},
    {ErrorCode::DeadlineAlreadyPassed, "DEADLINE_ALREADY_PASSED"},
    {ErrorCode::AmendmentNoChange, "AMENDMENT_NO_CHANGE"},
    {ErrorCode::LineageOverflow, "LINEAGE_OVERFLOW"},
    {ErrorCode::InsufficientCapacity, "INSUFFICIENT_CAPACITY"},
    {ErrorCode::HeadroomNotRevocable, "HEADROOM_NOT_REVOCABLE"},
    {ErrorCode::PolicyOverrideRequired, "POLICY_OVERRIDE_REQUIRED"},
    {ErrorCode::UnresolvableOvercommit, "UNRESOLVABLE_OVERCOMMIT"},
    {ErrorCode::AmendmentGenerationMismatch, "AMENDMENT_GENERATION_MISMATCH"},
    {ErrorCode::StaleReservationGeneration, "STALE_RESERVATION_GENERATION"},
    {ErrorCode::StaleAuthorityEpoch, "STALE_AUTHORITY_EPOCH"},
    {ErrorCode::StaleRevision, "STALE_REVISION"},
    {ErrorCode::AuthorityRequired, "AUTHORITY_REQUIRED"},
    {ErrorCode::ClaimantMismatch, "CLAIMANT_MISMATCH"},
    {ErrorCode::AttemptConflict, "ATTEMPT_CONFLICT"},
    {ErrorCode::AttemptRequired, "ATTEMPT_REQUIRED"},
    {ErrorCode::IncarnationMismatch, "INCARNATION_MISMATCH"},
    {ErrorCode::WriterEpochExhausted, "WRITER_EPOCH_EXHAUSTED"},
    {ErrorCode::RevisionExhausted, "REVISION_EXHAUSTED"},
    {ErrorCode::GenerationExhausted, "GENERATION_EXHAUSTED"},
    {ErrorCode::StoreNotFound, "STORE_NOT_FOUND"},
    {ErrorCode::StoreAlreadyExists, "STORE_ALREADY_EXISTS"},
    {ErrorCode::StoreNotEmpty, "STORE_NOT_EMPTY"},
    {ErrorCode::StoreCorrupt, "STORE_CORRUPT"},
    {ErrorCode::StoreIncompatible, "STORE_INCOMPATIBLE"},
    {ErrorCode::StoreLocked, "STORE_LOCKED"},
    {ErrorCode::StoreClosed, "STORE_CLOSED"},
    {ErrorCode::StoreReadOnly, "STORE_READ_ONLY"},
    {ErrorCode::StoreNotInitialised, "STORE_NOT_INITIALISED"},
    {ErrorCode::RecoveryRequired, "RECOVERY_REQUIRED"},
    {ErrorCode::RecoveryNotNeeded, "RECOVERY_NOT_NEEDED"},
    {ErrorCode::RecoveryFailed, "RECOVERY_FAILED"},
    {ErrorCode::NoValidGeneration, "NO_VALID_GENERATION"},
    {ErrorCode::HeadMissing, "HEAD_MISSING"},
    {ErrorCode::HeadCorrupt, "HEAD_CORRUPT"},
    {ErrorCode::IntegrityFailure, "INTEGRITY_FAILURE"},
    {ErrorCode::PathInvalid, "PATH_INVALID"},
    {ErrorCode::PathEscapesRoot, "PATH_ESCAPES_ROOT"},
    {ErrorCode::SymlinkRefused, "SYMLINK_REFUSED"},
    {ErrorCode::IoError, "IO_ERROR"},
    {ErrorCode::PermissionDenied, "PERMISSION_DENIED"},
    {ErrorCode::LockUnavailable, "LOCK_UNAVAILABLE"},
    {ErrorCode::InvariantViolation, "INVARIANT_VIOLATION"},
    {ErrorCode::AccountingMismatch, "ACCOUNTING_MISMATCH"},
    {ErrorCode::InternalError, "INTERNAL_ERROR"},
    {ErrorCode::Unsupported, "UNSUPPORTED"},
    {ErrorCode::Unavailable, "UNAVAILABLE"},
    {ErrorCode::Indeterminate, "INDETERMINATE"},
    {ErrorCode::CapacityAlreadyInstalled, "CAPACITY_ALREADY_INSTALLED"},
    {ErrorCode::NoCapacityInstalled, "NO_CAPACITY_INSTALLED"},
    {ErrorCode::OperationNotPermitted, "OPERATION_NOT_PERMITTED"},
    {ErrorCode::AttemptNotReplayable, "ATTEMPT_NOT_REPLAYABLE"},
};

constexpr std::size_t kCodeNameCount = sizeof(kCodeNames) / sizeof(kCodeNames[0]);

/// Highest enumerator value that must be present in the table.
constexpr ErrorCode kLastCode = ErrorCode::AttemptNotReplayable;

constexpr std::size_t kMaxErrorMessageBytes = 512;
constexpr std::size_t kMaxErrorSubjectBytes = 256;

}  // namespace

namespace detail {

std::size_t error_code_count() noexcept { return kCodeNameCount; }

bool error_vocabulary_complete() noexcept {
  // Every enumerator from Ok through the last declared code must appear exactly
  // once, and the table must be ordered by enumerator so that a duplicate or a
  // missing entry cannot hide behind a later match.
  const std::size_t expected_rows = static_cast<std::size_t>(kLastCode) + 1U;
  const bool correctly_sized = kCodeNameCount == expected_rows;
  if (!correctly_sized) {
    return false;
  }
  for (std::size_t index = 0; index < kCodeNameCount; ++index) {
    if (static_cast<std::size_t>(kCodeNames[index].first) != index) {
      return false;
    }
    if (kCodeNames[index].second.empty()) {
      return false;
    }
    for (std::size_t other = index + 1; other < kCodeNameCount; ++other) {
      if (kCodeNames[other].second == kCodeNames[index].second) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace detail

std::string_view error_code_name(ErrorCode code) noexcept {
  for (const Codes& entry : kCodeNames) {
    if (entry.first == code) {
      return entry.second;
    }
  }
  return "UNRECOGNISED_ERROR_CODE";
}

ErrorCategory error_category(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return ErrorCategory::Ok;

    case ErrorCode::InvalidArgument:
    case ErrorCode::MalformedIdentifier:
    case ErrorCode::IdentifierTooLong:
    case ErrorCode::TextTooLong:
    case ErrorCode::UnknownEnumToken:
    case ErrorCode::MissingField:
    case ErrorCode::DuplicateField:
    case ErrorCode::MalformedRecord:
    case ErrorCode::UnsupportedFormatVersion:
    case ErrorCode::CountMismatch:
    case ErrorCode::TruncatedInput:
    case ErrorCode::DigestMismatch:
    case ErrorCode::FieldNotAllowedHere:
    case ErrorCode::NonCanonicalOrder:
    case ErrorCode::SnapshotMalformed:
    case ErrorCode::SnapshotInconsistent:
    case ErrorCode::SnapshotEmpty:
    case ErrorCode::PoolKindMismatch:
    case ErrorCode::ClaimEmpty:
    case ErrorCode::ClaimAmountZero:
    case ErrorCode::ClaimDuplicatePool:
    case ErrorCode::ClaimSetUnsorted:
    case ErrorCode::InvalidInterval:
    case ErrorCode::DeadlineAlreadyPassed:
    case ErrorCode::AmendmentNoChange:
    case ErrorCode::AmendmentGenerationMismatch:
      return ErrorCategory::Argument;

    case ErrorCode::SnapshotNotFound:
    case ErrorCode::PoolUnknown:
    case ErrorCode::CapacityEvidenceStale:
    case ErrorCode::SourceGenerationStale:
    case ErrorCode::SourceGenerationConflict:
    case ErrorCode::InsufficientCapacity:
    case ErrorCode::HeadroomNotRevocable:
    case ErrorCode::PolicyOverrideRequired:
    case ErrorCode::UnresolvableOvercommit:
    case ErrorCode::CapacityAlreadyInstalled:
    case ErrorCode::NoCapacityInstalled:
      return ErrorCategory::Capacity;

    case ErrorCode::ReservationNotFound:
    case ErrorCode::ReservationAlreadyExists:
    case ErrorCode::ReservationNotActive:
    case ErrorCode::ReservationTerminal:
      return ErrorCategory::Reservation;

    case ErrorCode::StaleReservationGeneration:
    case ErrorCode::StaleAuthorityEpoch:
    case ErrorCode::StaleRevision:
    case ErrorCode::AuthorityRequired:
    case ErrorCode::ClaimantMismatch:
    case ErrorCode::AttemptConflict:
    case ErrorCode::AttemptRequired:
    case ErrorCode::AttemptNotReplayable:
    case ErrorCode::IncarnationMismatch:
    case ErrorCode::WriterEpochExhausted:
    case ErrorCode::RevisionExhausted:
    case ErrorCode::GenerationExhausted:
      return ErrorCategory::Authority;

    case ErrorCode::StoreNotFound:
    case ErrorCode::StoreAlreadyExists:
    case ErrorCode::StoreNotEmpty:
    case ErrorCode::StoreCorrupt:
    case ErrorCode::StoreIncompatible:
    case ErrorCode::StoreNotInitialised:
    case ErrorCode::RecoveryRequired:
    case ErrorCode::RecoveryNotNeeded:
    case ErrorCode::RecoveryFailed:
    case ErrorCode::NoValidGeneration:
    case ErrorCode::HeadMissing:
    case ErrorCode::HeadCorrupt:
    case ErrorCode::IntegrityFailure:
    case ErrorCode::PathInvalid:
    case ErrorCode::PathEscapesRoot:
    case ErrorCode::SymlinkRefused:
    case ErrorCode::IoError:
    case ErrorCode::PermissionDenied:
    case ErrorCode::LockUnavailable:
      return ErrorCategory::Persistence;

    case ErrorCode::StoreLocked:
    case ErrorCode::StoreClosed:
    case ErrorCode::StoreReadOnly:
    case ErrorCode::OperationNotPermitted:
      return ErrorCategory::Lifecycle;

    case ErrorCode::LimitExceeded:
    case ErrorCode::ArithmeticOverflow:
    case ErrorCode::LineageOverflow:
      return ErrorCategory::Limit;

    case ErrorCode::InvariantViolation:
    case ErrorCode::AccountingMismatch:
    case ErrorCode::InternalError:
    case ErrorCode::Unsupported:
    case ErrorCode::Unavailable:
    case ErrorCode::Indeterminate:
      return ErrorCategory::Internal;
  }
  return ErrorCategory::Internal;
}

std::string_view error_category_name(ErrorCategory category) noexcept {
  switch (category) {
    case ErrorCategory::Ok:
      return "OK";
    case ErrorCategory::Argument:
      return "ARGUMENT";
    case ErrorCategory::Capacity:
      return "CAPACITY";
    case ErrorCategory::Reservation:
      return "RESERVATION";
    case ErrorCategory::Authority:
      return "AUTHORITY";
    case ErrorCategory::Persistence:
      return "PERSISTENCE";
    case ErrorCategory::Lifecycle:
      return "LIFECYCLE";
    case ErrorCategory::Limit:
      return "LIMIT";
    case ErrorCategory::Internal:
      return "INTERNAL";
  }
  return "INTERNAL";
}

bool error_is_retryable(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::StoreLocked:
    case ErrorCode::LockUnavailable:
    case ErrorCode::IoError:
    case ErrorCode::Unavailable:
    case ErrorCode::Indeterminate:
      return true;
    default:
      return false;
  }
}

void Error::bound() {
  if (message_.size() > kMaxErrorMessageBytes) {
    message_.resize(kMaxErrorMessageBytes);
  }
  if (subject_.size() > kMaxErrorSubjectBytes) {
    subject_.resize(kMaxErrorSubjectBytes);
  }
}

std::string Error::to_string() const {
  std::string out(error_code_name(code_));
  out.append(": ");
  out.append(message_);
  if (!subject_.empty()) {
    out.append(" [subject=");
    out.append(subject_);
    out.push_back(']');
  }
  return out;
}

}  // namespace dccp::facility_capacity_reservation
