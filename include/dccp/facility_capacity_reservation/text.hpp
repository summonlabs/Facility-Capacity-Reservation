// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Bounded text primitives: canonical integers, canonical identifier text,
// unambiguous field encoding and the strict line reader used by the codec.
//
// Every function here treats its input as untrusted. Sizes are checked before
// anything is allocated, and nothing loops without a bound.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_TEXT_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_TEXT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_capacity_reservation/status.hpp"

namespace dccp::facility_capacity_reservation {

/// Upper bound applied to any single persisted or imported document.
inline constexpr std::size_t kMaxDocumentBytes = 64U * 1024U * 1024U;

/// Upper bound applied to any single identifier, reference, token or explanation.
inline constexpr std::size_t kMaxIdentifierBytes = 128U;
inline constexpr std::size_t kMaxTextBytes = 512U;

/// True when `value` is a single line of canonical integer text: digits only,
/// no sign, no leading zero unless the value is exactly "0".
bool is_canonical_unsigned(std::string_view value) noexcept;

/// Parses canonical unsigned decimal text. Rejects signs, whitespace, leading
/// zeros, empty input and anything that does not fit in std::uint64_t.
Result<std::uint64_t> parse_unsigned(std::string_view value);

/// Parses a canonical unsigned integer that must not exceed `maximum`.
Result<std::uint64_t> parse_unsigned_bounded(std::string_view value, std::uint64_t maximum);

/// Canonical decimal rendering of an unsigned value.
std::string format_unsigned(std::uint64_t value);

/// True when `value` is a usable identifier: 1..kMaxIdentifierBytes bytes from
/// [A-Za-z0-9._-], starting with an alphanumeric character.
///
/// The restricted alphabet is deliberate. Identifiers are echoed into canonical
/// documents, CLI output, error subjects and diagnostic pool keys, so path
/// separators, whitespace, control bytes, NUL, quotes, Unicode confusables and —
/// because it is both a field separator in the tool's grammar and a Windows
/// drive qualifier — the colon are all refused at the boundary rather than
/// escaped later. A colon can therefore never make a kind:scope:amount grammar
/// or a diagnostic pool key ambiguous.
bool is_valid_identifier(std::string_view value) noexcept;

/// Validates an identifier and its length bound.
Result<void> validate_identifier(std::string_view value, std::string_view what);

/// True when `value` is lowercase hexadecimal of exactly `digits` characters.
bool is_lower_hex(std::string_view value, std::size_t digits) noexcept;

/// Reads exactly `2 * kMaxIdentifierBytes` of lowercase hex into bytes.
Result<std::vector<std::uint8_t>> parse_hex_bytes(std::string_view value, std::size_t expected_bytes);

/// Renders bytes as lowercase hex.
std::string format_hex(const std::uint8_t* data, std::size_t size);
std::string format_hex(const std::vector<std::uint8_t>& data);

/// Encodes arbitrary text so that it survives a single-line, `|`-delimited,
/// `=`-separated canonical record without ambiguity.
///
/// Bytes in [A-Za-z0-9._:/-] are emitted verbatim; every other byte becomes
/// "%XX" with uppercase hex digits. The encoding is injective and is refused
/// (rather than truncated) when the result would exceed `max_output`.
Result<std::string> encode_field(std::string_view value, std::size_t max_output);

/// Reverses encode_field. Rejects malformed escapes and non-canonical escapes
/// (a byte that encode_field would have emitted verbatim must appear verbatim).
Result<std::string> decode_field(std::string_view value, std::size_t max_output);

/// Splits a line on a single separator, with an explicit bound on the number of
/// resulting fields. Empty fields are preserved; the separator is never
/// optional and a trailing separator produces a final empty field.
Result<std::vector<std::string_view>> split_fields(std::string_view line, char separator, std::size_t max_fields);

/// Splits a whole canonical document into lines on '\n', rejecting NUL bytes,
/// bare CR and a final line that is not terminated by '\n'.
Result<std::vector<std::string_view>> split_lines(std::string_view document, std::size_t max_lines);

/// Replaces every non-printable byte so a message is safe to print. Used for
/// diagnostics only: never for authoritative text.
std::string sanitize_for_display(std::string_view value);

}  // namespace dccp::facility_capacity_reservation

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_TEXT_HPP
