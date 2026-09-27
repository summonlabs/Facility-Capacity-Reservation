// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity_reservation/strong_id.hpp"

#include <atomic>
#include <chrono>
#include <random>

#include "dccp/facility_capacity_reservation/digest.hpp"

namespace dccp::facility_capacity_reservation {
namespace {

std::uint64_t entropy_word() {
  // std::random_device is the standard-library entropy source on every platform
  // this project is exercised on. It is mixed with the wall clock and a
  // monotonic counter so that two incarnations created in the same process can
  // never collide even if the entropy source repeats.
  static std::atomic<std::uint64_t> counter{0};
  static std::random_device device;
  static const std::uint64_t process_salt =
      (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());

  const std::uint64_t now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  std::uint64_t mixed = now ^ process_salt ^ (counter.fetch_add(1, std::memory_order_relaxed) * 0x9E3779B97F4A7C15ULL);
  mixed ^= static_cast<std::uint64_t>(device());
  return mixed;
}

std::uint8_t hex_nibble(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0');
  }
  return static_cast<std::uint8_t>(value - 'a' + 10);
}

}  // namespace

Incarnation Incarnation::generate() {
  std::string material;
  material.reserve(64);
  for (int index = 0; index < 4; ++index) {
    const std::uint64_t word = entropy_word();
    std::uint8_t bytes[8];
    for (int byte = 0; byte < 8; ++byte) {
      bytes[byte] = static_cast<std::uint8_t>((word >> (byte * 8)) & 0xFFU);
    }
    material.append(format_hex(bytes, sizeof(bytes)));
  }
  const std::string digest = sha256_hex(material);

  Incarnation incarnation;
  incarnation.bytes_.resize(kBytes);
  for (std::size_t index = 0; index < kBytes; ++index) {
    incarnation.bytes_[index] =
        static_cast<std::uint8_t>((hex_nibble(digest[index * 2]) << 4) | hex_nibble(digest[index * 2 + 1]));
  }
  return incarnation;
}

}  // namespace dccp::facility_capacity_reservation
