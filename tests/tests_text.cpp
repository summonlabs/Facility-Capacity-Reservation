// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Bounded text primitives: canonical integers, identifiers, field encoding and
// strict line splitting. Every one of these is on the untrusted-input path.

#include <string>
#include <vector>

#include "dccp/facility_capacity_reservation/text.hpp"
#include "test_framework.hpp"

namespace fcr = dccp::facility_capacity_reservation;

FCR_TEST(text, canonical_unsigned_accepts_only_canonical_decimal) {
  FCR_CHECK(fcr::is_canonical_unsigned("0"));
  FCR_CHECK(fcr::is_canonical_unsigned("7"));
  FCR_CHECK(fcr::is_canonical_unsigned("18446744073709551615"));
  FCR_CHECK(!fcr::is_canonical_unsigned(""));
  FCR_CHECK(!fcr::is_canonical_unsigned("00"));
  FCR_CHECK(!fcr::is_canonical_unsigned("01"));
  FCR_CHECK(!fcr::is_canonical_unsigned("+1"));
  FCR_CHECK(!fcr::is_canonical_unsigned("-1"));
  FCR_CHECK(!fcr::is_canonical_unsigned(" 1"));
  FCR_CHECK(!fcr::is_canonical_unsigned("1 "));
  FCR_CHECK(!fcr::is_canonical_unsigned("1e3"));
  FCR_CHECK(!fcr::is_canonical_unsigned("0x10"));
  // The predicate is lexical: a twenty-digit literal is canonical text even when
  // its value does not fit, which is what the parser reports separately.
  FCR_CHECK(fcr::is_canonical_unsigned("18446744073709551616"));
}

FCR_TEST(text, parse_unsigned_reports_overflow_and_malformed_input_separately) {
  FCR_CHECK_EQ(fcr::parse_unsigned("18446744073709551615").value(), 0xFFFF'FFFF'FFFF'FFFFULL);
  FCR_CHECK_ERROR(fcr::parse_unsigned("18446744073709551616"), fcr::ErrorCode::ArithmeticOverflow);
  FCR_CHECK_ERROR(fcr::parse_unsigned("000000000000000000001"), fcr::ErrorCode::LimitExceeded);
  FCR_CHECK_ERROR(fcr::parse_unsigned("twelve"), fcr::ErrorCode::MalformedRecord);
  FCR_CHECK_ERROR(fcr::parse_unsigned(""), fcr::ErrorCode::MalformedRecord);
}

FCR_TEST(text, parse_unsigned_bounded_checks_the_bound) {
  FCR_CHECK_EQ(fcr::parse_unsigned_bounded("100", 100).value(), 100ULL);
  FCR_CHECK_ERROR(fcr::parse_unsigned_bounded("101", 100), fcr::ErrorCode::LimitExceeded);
}

FCR_TEST(text, format_unsigned_is_canonical) {
  FCR_CHECK_EQ(fcr::format_unsigned(0), std::string("0"));
  FCR_CHECK_EQ(fcr::format_unsigned(9), std::string("9"));
  FCR_CHECK_EQ(fcr::format_unsigned(10), std::string("10"));
  FCR_CHECK_EQ(fcr::format_unsigned(0xFFFF'FFFF'FFFF'FFFFULL), std::string("18446744073709551615"));
  for (std::uint64_t value = 0; value < 5'000; ++value) {
    const std::string rendered = fcr::format_unsigned(value * 1'000'003ULL);
    FCR_CHECK(fcr::is_canonical_unsigned(rendered));
    FCR_CHECK_EQ(fcr::parse_unsigned(rendered).value(), value * 1'000'003ULL);
  }
}

FCR_TEST(text, identifiers_reject_everything_that_could_escape_a_context) {
  FCR_CHECK(fcr::is_valid_identifier("res-1"));
  FCR_CHECK(fcr::is_valid_identifier("A.b_c-d.e"));
  FCR_CHECK(fcr::is_valid_identifier("0"));
  FCR_CHECK(!fcr::is_valid_identifier(""));
  FCR_CHECK(!fcr::is_valid_identifier("-leading"));
  FCR_CHECK(!fcr::is_valid_identifier(".hidden"));
  FCR_CHECK(!fcr::is_valid_identifier(".."));
  FCR_CHECK(!fcr::is_valid_identifier("."));
  FCR_CHECK(!fcr::is_valid_identifier("a/b"));
  FCR_CHECK(!fcr::is_valid_identifier("a\\b"));
  FCR_CHECK(!fcr::is_valid_identifier("C:name"));
  FCR_CHECK(!fcr::is_valid_identifier("a b"));
  FCR_CHECK(!fcr::is_valid_identifier("a\tb"));
  FCR_CHECK(!fcr::is_valid_identifier(std::string("a\0b", 3)));
  FCR_CHECK(!fcr::is_valid_identifier("caf\xc3\xa9"));
  FCR_CHECK(!fcr::is_valid_identifier("a=b"));
  FCR_CHECK(!fcr::is_valid_identifier("a:b"));
  FCR_CHECK(!fcr::is_valid_identifier("a|b"));
  FCR_CHECK(!fcr::is_valid_identifier("a\nb"));
  FCR_CHECK(!fcr::is_valid_identifier(std::string(fcr::kMaxIdentifierBytes + 1, 'a')));
  FCR_CHECK(fcr::is_valid_identifier(std::string(fcr::kMaxIdentifierBytes, 'a')));
}

FCR_TEST(text, validate_identifier_distinguishes_the_rejection_reasons) {
  FCR_CHECK_ERROR(fcr::validate_identifier("", "scope"), fcr::ErrorCode::MissingField);
  FCR_CHECK_ERROR(fcr::validate_identifier(std::string(200, 'a'), "scope"), fcr::ErrorCode::IdentifierTooLong);
  FCR_CHECK_ERROR(fcr::validate_identifier("bad name", "scope"), fcr::ErrorCode::MalformedIdentifier);
  FCR_CHECK(fcr::validate_identifier("good-name", "scope").has_value());
}

FCR_TEST(text, field_encoding_round_trips_and_stays_canonical) {
  const std::vector<std::string> samples = {
      "", "plain", "with space", "pipe|inside", "equals=inside", "new\nline", "tab\there", "percent%20",
      std::string("nul\0byte", 8), "\x01\x02\x03", "unicode-\xc3\xa9", "mixed |= \r\n"};
  for (const std::string& sample : samples) {
    const fcr::Result<std::string> encoded = fcr::encode_field(sample, 4096);
    FCR_REQUIRE(encoded.has_value());
    FCR_CHECK(encoded->find('|') == std::string::npos);
    FCR_CHECK(encoded->find('\n') == std::string::npos);
    FCR_CHECK(encoded->find('=') == std::string::npos);
    const fcr::Result<std::string> decoded = fcr::decode_field(*encoded, 4096);
    FCR_REQUIRE(decoded.has_value());
    FCR_CHECK_EQ(*decoded, sample);
  }
}

FCR_TEST(text, field_decoding_refuses_non_canonical_and_malformed_escapes) {
  FCR_CHECK_ERROR(fcr::decode_field("%", 64), fcr::ErrorCode::MalformedRecord);
  FCR_CHECK_ERROR(fcr::decode_field("%A", 64), fcr::ErrorCode::MalformedRecord);
  FCR_CHECK_ERROR(fcr::decode_field("%zz", 64), fcr::ErrorCode::MalformedRecord);
  FCR_CHECK_ERROR(fcr::decode_field("%2f", 64), fcr::ErrorCode::MalformedRecord);
  // "/" is emitted verbatim, so escaping it is not canonical.
  FCR_CHECK_ERROR(fcr::decode_field("%2F", 64), fcr::ErrorCode::NonCanonicalOrder);
  // A byte that must be escaped may not appear raw.
  FCR_CHECK_ERROR(fcr::decode_field("a b", 64), fcr::ErrorCode::MalformedRecord);
  FCR_CHECK_EQ(fcr::decode_field("%20", 64).value(), std::string(" "));
}

FCR_TEST(text, field_encoding_is_bounded_before_it_grows) {
  const std::string sample(3'000, '|');
  FCR_CHECK_ERROR(fcr::encode_field(sample, 512), fcr::ErrorCode::TextTooLong);
  FCR_CHECK_ERROR(fcr::decode_field("%7C%7C%7C", 1), fcr::ErrorCode::TextTooLong);
}

FCR_TEST(text, split_fields_preserves_empties_and_bounds_the_count) {
  const fcr::Result<std::vector<std::string_view>> parts = fcr::split_fields("a||b|", '|', 8);
  FCR_REQUIRE(parts.has_value());
  FCR_REQUIRE(parts->size() == 4);
  FCR_CHECK_EQ((*parts)[0], std::string_view("a"));
  FCR_CHECK_EQ((*parts)[1], std::string_view(""));
  FCR_CHECK_EQ((*parts)[2], std::string_view("b"));
  FCR_CHECK_EQ((*parts)[3], std::string_view(""));
  FCR_CHECK_ERROR(fcr::split_fields("a|b|c", '|', 2), fcr::ErrorCode::LimitExceeded);
}

FCR_TEST(text, split_lines_requires_a_terminated_document) {
  const fcr::Result<std::vector<std::string_view>> lines = fcr::split_lines("a\nb\n", 8);
  FCR_REQUIRE(lines.has_value());
  FCR_REQUIRE(lines->size() == 2);
  FCR_CHECK_EQ((*lines)[1], std::string_view("b"));
  FCR_CHECK_ERROR(fcr::split_lines("a\nb", 8), fcr::ErrorCode::TruncatedInput);
  FCR_CHECK_ERROR(fcr::split_lines("", 8), fcr::ErrorCode::TruncatedInput);
  FCR_CHECK_ERROR(fcr::split_lines("a\r\n", 8), fcr::ErrorCode::MalformedRecord);
  FCR_CHECK_ERROR(fcr::split_lines(std::string("a\0b\n", 4), 8), fcr::ErrorCode::MalformedRecord);
  FCR_CHECK_ERROR(fcr::split_lines("a\nb\nc\n", 2), fcr::ErrorCode::LimitExceeded);
}

FCR_TEST(text, sanitize_for_display_removes_control_bytes_and_bounds_the_result) {
  const std::string dirty = std::string("ok\x01\x02") + std::string(2'000, 'z');
  const std::string clean = fcr::sanitize_for_display(dirty);
  FCR_CHECK(clean.size() <= 515);
  FCR_CHECK(clean.find('\x01') == std::string::npos);
  FCR_CHECK_EQ(fcr::sanitize_for_display("plain"), std::string("plain"));
}

FCR_TEST(text, hexadecimal_helpers_reject_uppercase_and_wrong_lengths) {
  FCR_CHECK(fcr::is_lower_hex("0123456789abcdef", 16));
  FCR_CHECK(!fcr::is_lower_hex("0123456789ABCDEF", 16));
  FCR_CHECK(!fcr::is_lower_hex("0123", 16));
  FCR_CHECK(fcr::parse_hex_bytes("00ff", 2).has_value());
  FCR_CHECK_ERROR(fcr::parse_hex_bytes("00ff", 3), fcr::ErrorCode::CountMismatch);
  FCR_CHECK_ERROR(fcr::parse_hex_bytes("00gg", 2), fcr::ErrorCode::MalformedRecord);
  const std::vector<std::uint8_t> bytes{0x00, 0x0F, 0xF0, 0xFF};
  FCR_CHECK_EQ(fcr::format_hex(bytes), std::string("000ff0ff"));
}
