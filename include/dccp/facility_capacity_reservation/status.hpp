// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Stable outcome vocabulary for Facility Capacity Reservation.
//
// Every fallible operation reports its outcome through Result<T>. Expected
// failure modes are never signalled with exceptions: a caller that wants to
// branch on the kind of failure reads the stable code, and a caller that wants
// to explain it to a human reads the message. The codes below are a public
// contract and are appended to, never renumbered or repurposed.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_STATUS_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_STATUS_HPP

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace dccp::facility_capacity_reservation {

/// Machine-readable outcome codes.
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // --- Request shape and encoding (untrusted input) -----------------------
  InvalidArgument,
  MalformedIdentifier,
  IdentifierTooLong,
  TextTooLong,
  UnknownEnumToken,
  MissingField,
  DuplicateField,
  MalformedRecord,
  UnsupportedFormatVersion,
  CountMismatch,
  TruncatedInput,
  DigestMismatch,
  FieldNotAllowedHere,
  NonCanonicalOrder,
  LimitExceeded,
  ArithmeticOverflow,

  // --- Capacity snapshot consumption -------------------------------------
  SnapshotNotFound,
  SnapshotMalformed,
  SnapshotInconsistent,
  SnapshotEmpty,
  PoolUnknown,
  PoolKindMismatch,
  CapacityEvidenceStale,
  SourceGenerationStale,
  SourceGenerationConflict,

  // --- Reservation structure and semantics --------------------------------
  ReservationNotFound,
  ReservationAlreadyExists,
  ReservationNotActive,
  ReservationTerminal,
  ClaimEmpty,
  ClaimAmountZero,
  ClaimDuplicatePool,
  ClaimSetUnsorted,
  InvalidInterval,
  DeadlineAlreadyPassed,
  AmendmentNoChange,
  LineageOverflow,
  InsufficientCapacity,
  HeadroomNotRevocable,
  PolicyOverrideRequired,
  UnresolvableOvercommit,
  AmendmentGenerationMismatch,

  // --- Authority, generations, attempts -----------------------------------
  StaleReservationGeneration,
  StaleAuthorityEpoch,
  StaleRevision,
  AuthorityRequired,
  ClaimantMismatch,
  AttemptConflict,
  AttemptRequired,
  IncarnationMismatch,
  WriterEpochExhausted,
  RevisionExhausted,
  GenerationExhausted,

  // --- Store lifecycle and persistence ------------------------------------
  StoreNotFound,
  StoreAlreadyExists,
  StoreNotEmpty,
  StoreCorrupt,
  StoreIncompatible,
  StoreLocked,
  StoreClosed,
  StoreReadOnly,
  StoreNotInitialised,
  RecoveryRequired,
  RecoveryNotNeeded,
  RecoveryFailed,
  NoValidGeneration,
  HeadMissing,
  HeadCorrupt,
  IntegrityFailure,
  PathInvalid,
  PathEscapesRoot,
  SymlinkRefused,
  IoError,
  PermissionDenied,
  LockUnavailable,

  // --- Library defects ----------------------------------------------------
  InvariantViolation,
  AccountingMismatch,
  InternalError,
  Unsupported,
  Unavailable,
  Indeterminate,

  // --- Appended after the initial vocabulary ------------------------------
  // Existing values are never renumbered, so a new code only ever extends the
  // list.
  CapacityAlreadyInstalled,
  NoCapacityInstalled,
  OperationNotPermitted,
  AttemptNotReplayable,
};

/// Coarse classification, for callers that branch on the kind of failure.
enum class ErrorCategory : std::uint8_t {
  Ok = 0,
  Argument,     // caller-supplied or untrusted input was rejected
  Capacity,     // capacity evidence or capacity accounting refused the operation
  Reservation,  // the reservation structure or lifecycle refused the operation
  Authority,    // a generation / epoch / revision / attempt precondition failed
  Persistence,  // durable state is missing, corrupt, incompatible or unwritable
  Lifecycle,    // the store is locked, closed, read-only or in the wrong state
  Limit,        // a configured bound was exceeded
  Internal,     // defect in the library
};

/// Stable textual name of an error code (upper snake case).
std::string_view error_code_name(ErrorCode code) noexcept;

/// Category of an error code.
ErrorCategory error_category(ErrorCode code) noexcept;

/// Human-readable name of a category.
std::string_view error_category_name(ErrorCategory category) noexcept;

/// True when the code reports a condition that a later retry could clear.
bool error_is_retryable(ErrorCode code) noexcept;

/// An error value: stable code, bounded explanation, optional subject.
class Error {
 public:
  Error() noexcept = default;

  Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) { bound(); }

  ErrorCode code() const noexcept { return code_; }
  ErrorCategory category() const noexcept { return error_category(code_); }
  const std::string& message() const noexcept { return message_; }

  /// Identifier of the object the error is about, when one exists.
  const std::string& subject() const noexcept { return subject_; }

  Error& with_subject(std::string subject) {
    subject_ = std::move(subject);
    bound();
    return *this;
  }

  bool ok() const noexcept { return code_ == ErrorCode::Ok; }

  /// "CODE: message" (plus " [subject=...]" when a subject is present).
  std::string to_string() const;

 private:
  /// Keeps an error value small even when it was built from untrusted text: a
  /// rejection message must never itself be an unbounded allocation.
  void bound();

  ErrorCode code_ = ErrorCode::Ok;
  std::string message_;
  std::string subject_;
};

/// Result of an operation that yields a value of type T or an Error.
///
/// The library never uses exceptions for expected failure modes. value() throws
/// std::logic_error only on programmer error (dereferencing a failed Result).
template <class T>
class Result {
 public:
  using value_type = T;

  Result(T value) : value_(std::move(value)) {}
  Result(Error error) : error_(normalize(std::move(error))) {}

  bool has_value() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return has_value(); }

  T& value() & {
    require_value();
    return *value_;
  }
  const T& value() const& {
    require_value();
    return *value_;
  }
  T&& value() && {
    require_value();
    return std::move(*value_);
  }

  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

  const Error& error() const noexcept { return error_; }

 private:
  static Error normalize(Error error) {
    if (error.ok()) {
      return Error(ErrorCode::InternalError, "result constructed without a value or an error");
    }
    return error;
  }

  void require_value() const {
    if (!value_.has_value()) {
      throw std::logic_error("facility_capacity_reservation: Result has no value: " + error_.to_string());
    }
  }

  std::optional<T> value_;
  Error error_;
};

/// Result specialization for operations that produce no value.
template <>
class Result<void> {
 public:
  Result() noexcept = default;
  Result(Error error) : error_(normalize(std::move(error))) {}

  static Result success() noexcept { return Result(); }

  bool has_value() const noexcept { return error_.ok(); }
  explicit operator bool() const noexcept { return has_value(); }

  const Error& error() const noexcept { return error_; }

 private:
  static Error normalize(Error error) {
    if (error.ok()) {
      return Error(ErrorCode::InternalError, "result constructed without a value or an error");
    }
    return error;
  }

  Error error_;
};

inline Error make_error(ErrorCode code, std::string message) { return Error(code, std::move(message)); }

inline Result<void> ok() noexcept { return Result<void>(); }

}  // namespace dccp::facility_capacity_reservation

/// Propagate a failed Result out of the current function.
///
/// FCR_TRY(name, expression) declares "name" bound to the successful value and
/// returns the error if the expression failed.
#define FCR_TRY(value_name, expression)         \
  auto value_name##_fcr_result = (expression);  \
  if (!value_name##_fcr_result.has_value()) {   \
    return value_name##_fcr_result.error();     \
  }                                             \
  auto& value_name = *value_name##_fcr_result

/// Propagate a failed void Result out of the current function.
#define FCR_TRYV(expression)          \
  do {                                \
    auto fcr_result_ = (expression);  \
    if (!fcr_result_.has_value()) {   \
      return fcr_result_.error();     \
    }                                 \
  } while (false)

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_STATUS_HPP
