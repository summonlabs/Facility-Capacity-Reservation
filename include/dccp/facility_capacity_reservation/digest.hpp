// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// SHA-256 (FIPS 180-4), implemented here so that the integrity of persisted and
// imported documents has no third-party dependency.
//
// The digest is an integrity check over bytes this library wrote or read. It is
// not a signature and is not treated as evidence of authorship.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_DIGEST_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_DIGEST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace dccp::facility_capacity_reservation {

/// Streaming SHA-256.
class Sha256 {
 public:
  static constexpr std::size_t kDigestBytes = 32;
  static constexpr std::size_t kDigestHexDigits = kDigestBytes * 2;

  Sha256() noexcept { reset(); }

  void reset() noexcept;

  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }

  /// Finalizes and writes the 32 digest bytes. The object must be reset before
  /// it is reused.
  void finish(std::uint8_t out[kDigestBytes]) noexcept;

  /// Finalizes and returns the digest as lowercase hexadecimal.
  std::string finish_hex();

 private:
  void compress(const std::uint8_t block[64]) noexcept;

  std::uint32_t state_[8];
  std::uint64_t bit_length_;
  std::uint8_t buffer_[64];
  std::size_t buffered_;
};

/// One-shot lowercase hexadecimal SHA-256 of a byte range.
std::string sha256_hex(std::string_view text);
std::string sha256_hex(const void* data, std::size_t size);

}  // namespace dccp::facility_capacity_reservation

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_DIGEST_HPP
