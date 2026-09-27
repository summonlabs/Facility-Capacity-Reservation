// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity_reservation/text.hpp"

#include <array>
#include <cstdio>

namespace dccp::facility_capacity_reservation {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr char kHexUpper[] = "0123456789ABCDEF";

bool is_identifier_char(char value) noexcept {
  const bool digit = value >= '0' && value <= '9';
  const bool upper = value >= 'A' && value <= 'Z';
  const bool lower = value >= 'a' && value <= 'z';
  // '.' and '_' are the only punctuation beyond the hyphen: ':' is deliberately
  // excluded because it is the separator of the tool's KIND:SCOPE:AMOUNT
  // grammar and of the diagnostic pool key, and a character that can appear
  // inside a token and also separate tokens makes every grammar ambiguous.
  return digit || upper || lower || value == '.' || value == '_' || value == '-';
}

bool is_verbatim_field_char(unsigned char value) noexcept {
  const bool digit = value >= '0' && value <= '9';
  const bool upper = value >= 'A' && value <= 'Z';
  const bool lower = value >= 'a' && value <= 'z';
  return digit || upper || lower || value == '.' || value == '_' || value == ':' || value == '/' || value == '-';
}

int hex_value(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  return -1;
}

}  // namespace

bool is_canonical_unsigned(std::string_view value) noexcept {
  if (value.empty() || value.size() > 20) {
    return false;
  }
  for (const char character : value) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  if (value.size() > 1 && value.front() == '0') {
    return false;
  }
  return true;
}

Result<std::uint64_t> parse_unsigned(std::string_view value) {
  if (!is_canonical_unsigned(value)) {
    if (value.size() > 20) {
      return Error(ErrorCode::LimitExceeded, "integer literal is longer than 20 digits");
    }
    return Error(ErrorCode::MalformedRecord, "integer literal is not canonical unsigned decimal");
  }
  std::uint64_t result = 0;
  for (const char character : value) {
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (result > (0xFFFF'FFFF'FFFF'FFFFULL - digit) / 10ULL) {
      return Error(ErrorCode::ArithmeticOverflow, "integer literal does not fit in 64 bits");
    }
    result = result * 10ULL + digit;
  }
  return result;
}

Result<std::uint64_t> parse_unsigned_bounded(std::string_view value, std::uint64_t maximum) {
  FCR_TRY(parsed, parse_unsigned(value));
  if (parsed > maximum) {
    return Error(ErrorCode::LimitExceeded, "integer literal exceeds the permitted maximum");
  }
  return parsed;
}

std::string format_unsigned(std::uint64_t value) {
  if (value == 0) {
    return std::string("0");
  }
  std::array<char, 20> buffer{};
  std::size_t position = buffer.size();
  while (value != 0) {
    buffer[--position] = static_cast<char>('0' + static_cast<int>(value % 10ULL));
    value /= 10ULL;
  }
  return std::string(buffer.data() + position, buffer.size() - position);
}

bool is_valid_identifier(std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaxIdentifierBytes) {
    return false;
  }
  const char first = value.front();
  const bool first_alnum = (first >= '0' && first <= '9') || (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
  if (!first_alnum) {
    return false;
  }
  for (const char character : value) {
    if (!is_identifier_char(character)) {
      return false;
    }
  }
  // "." and ".." cannot occur because the first character must be alphanumeric,
  // so an identifier can never name a directory traversal component.
  return true;
}

Result<void> validate_identifier(std::string_view value, std::string_view what) {
  if (value.empty()) {
    return Error(ErrorCode::MissingField, std::string(what) + " must not be empty");
  }
  if (value.size() > kMaxIdentifierBytes) {
    return Error(ErrorCode::IdentifierTooLong,
                 std::string(what) + " is longer than " + format_unsigned(kMaxIdentifierBytes) + " bytes");
  }
  if (!is_valid_identifier(value)) {
    return Error(ErrorCode::MalformedIdentifier,
                 std::string(what) +
                     " must contain only [A-Za-z0-9._-] and start with an alphanumeric character");
  }
  return ok();
}

bool is_lower_hex(std::string_view value, std::size_t digits) noexcept {
  if (value.size() != digits) {
    return false;
  }
  for (const char character : value) {
    if (hex_value(character) < 0) {
      return false;
    }
  }
  return true;
}

Result<std::vector<std::uint8_t>> parse_hex_bytes(std::string_view value, std::size_t expected_bytes) {
  if (value.size() != expected_bytes * 2) {
    return Error(ErrorCode::CountMismatch, "hexadecimal field has the wrong length");
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve(expected_bytes);
  for (std::size_t index = 0; index < expected_bytes; ++index) {
    const int high = hex_value(value[index * 2]);
    const int low = hex_value(value[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return Error(ErrorCode::MalformedRecord, "hexadecimal field contains a non-hexadecimal character");
    }
    bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return bytes;
}

std::string format_hex(const std::uint8_t* data, std::size_t size) {
  std::string out;
  out.resize(size * 2);
  for (std::size_t index = 0; index < size; ++index) {
    out[index * 2] = kHexDigits[data[index] >> 4];
    out[index * 2 + 1] = kHexDigits[data[index] & 0x0FU];
  }
  return out;
}

std::string format_hex(const std::vector<std::uint8_t>& data) { return format_hex(data.data(), data.size()); }

Result<std::string> encode_field(std::string_view value, std::size_t max_output) {
  std::string out;
  out.reserve(value.size());
  for (const char character : value) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (is_verbatim_field_char(byte)) {
      out.push_back(character);
    } else {
      if (out.size() + 3 > max_output) {
        return Error(ErrorCode::TextTooLong, "encoded field would exceed the permitted length");
      }
      out.push_back('%');
      out.push_back(kHexUpper[byte >> 4]);
      out.push_back(kHexUpper[byte & 0x0FU]);
    }
    if (out.size() > max_output) {
      return Error(ErrorCode::TextTooLong, "encoded field would exceed the permitted length");
    }
  }
  return out;
}

Result<std::string> decode_field(std::string_view value, std::size_t max_output) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    const unsigned char byte = static_cast<unsigned char>(value[index]);
    if (byte == '%') {
      if (index + 2 >= value.size()) {
        return Error(ErrorCode::MalformedRecord, "field ends inside a percent escape");
      }
      const char high_char = value[index + 1];
      const char low_char = value[index + 2];
      int high = -1;
      int low = -1;
      if (high_char >= '0' && high_char <= '9') {
        high = high_char - '0';
      } else if (high_char >= 'A' && high_char <= 'F') {
        high = high_char - 'A' + 10;
      }
      if (low_char >= '0' && low_char <= '9') {
        low = low_char - '0';
      } else if (low_char >= 'A' && low_char <= 'F') {
        low = low_char - 'A' + 10;
      }
      if (high < 0 || low < 0) {
        return Error(ErrorCode::MalformedRecord, "percent escape must use uppercase hexadecimal digits");
      }
      const unsigned char decoded = static_cast<unsigned char>((high << 4) | low);
      // A byte that encode_field would have emitted verbatim must appear
      // verbatim: an escaped verbatim byte is not canonical.
      if (is_verbatim_field_char(decoded)) {
        return Error(ErrorCode::NonCanonicalOrder, "percent escape encodes a byte that is emitted verbatim");
      }
      // Both hexadecimal digits are consumed by the escape; without advancing
      // past them they would be emitted again as literal text.
      index += 2;
      out.push_back(static_cast<char>(decoded));
    } else {
      if (!is_verbatim_field_char(byte)) {
        return Error(ErrorCode::MalformedRecord, "field contains a character that must be percent-escaped");
      }
      out.push_back(static_cast<char>(byte));
    }
    if (out.size() > max_output) {
      return Error(ErrorCode::TextTooLong, "decoded field exceeds the permitted length");
    }
  }
  return out;
}

Result<std::vector<std::string_view>> split_fields(std::string_view line, char separator, std::size_t max_fields) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  for (;;) {
    if (fields.size() >= max_fields) {
      return Error(ErrorCode::LimitExceeded, "record declares more fields than the configured maximum");
    }
    const std::size_t position = line.find(separator, start);
    if (position == std::string_view::npos) {
      fields.push_back(line.substr(start));
      break;
    }
    fields.push_back(line.substr(start, position - start));
    start = position + 1;
  }
  return fields;
}

Result<std::vector<std::string_view>> split_lines(std::string_view document, std::size_t max_lines) {
  if (document.size() > kMaxDocumentBytes) {
    return Error(ErrorCode::LimitExceeded, "document exceeds the configured maximum size");
  }
  if (document.empty()) {
    return Error(ErrorCode::TruncatedInput, "document is empty");
  }
  if (document.back() != '\n') {
    return Error(ErrorCode::TruncatedInput, "document must end with a newline");
  }
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (start < document.size()) {
    if (lines.size() >= max_lines) {
      return Error(ErrorCode::LimitExceeded, "document has more lines than the configured maximum");
    }
    const std::size_t position = document.find('\n', start);
    const std::string_view line = document.substr(start, position - start);
    if (line.find('\0') != std::string_view::npos) {
      return Error(ErrorCode::MalformedRecord, "document contains a NUL byte");
    }
    if (line.find('\r') != std::string_view::npos) {
      return Error(ErrorCode::MalformedRecord, "document contains a carriage return");
    }
    lines.push_back(line);
    start = position + 1;
  }
  return lines;
}

std::string sanitize_for_display(std::string_view value) {
  constexpr std::size_t kMaxDisplayBytes = 512;
  std::string out;
  out.reserve(value.size());
  for (const char character : value) {
    if (out.size() >= kMaxDisplayBytes) {
      out.append("...");
      break;
    }
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte >= 0x20 && byte < 0x7F) {
      out.push_back(character);
    } else {
      out.push_back('?');
    }
  }
  return out;
}

}  // namespace dccp::facility_capacity_reservation
