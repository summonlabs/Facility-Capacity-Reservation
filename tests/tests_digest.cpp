// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// SHA-256 against the published FIPS 180-4 test vectors, plus the streaming
// behaviour the store relies on.

#include <string>
#include <vector>

#include "dccp/facility_capacity_reservation/digest.hpp"
#include "test_framework.hpp"

namespace fcr = dccp::facility_capacity_reservation;

FCR_TEST(digest, known_answer_vectors) {
  // FIPS 180-4 / NIST published vectors.
  FCR_CHECK_EQ(fcr::sha256_hex(""),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  FCR_CHECK_EQ(fcr::sha256_hex("abc"),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  FCR_CHECK_EQ(
      fcr::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  FCR_CHECK_EQ(
      fcr::sha256_hex(
          "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
      std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
}

FCR_TEST(digest, million_byte_vector) {
  std::string million(1'000'000, 'a');
  FCR_CHECK_EQ(fcr::sha256_hex(million),
               std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

FCR_TEST(digest, streaming_matches_one_shot_for_every_split_point) {
  const std::string sample =
      "the quick brown fox jumps over the lazy dog; facility capacity reservation; 0123456789";
  const std::string expected = fcr::sha256_hex(sample);
  for (std::size_t split = 0; split <= sample.size(); ++split) {
    fcr::Sha256 hasher;
    hasher.update(std::string_view(sample).substr(0, split));
    hasher.update(std::string_view(sample).substr(split));
    FCR_CHECK_EQ(hasher.finish_hex(), expected);
  }
}

FCR_TEST(digest, block_boundary_lengths_are_handled) {
  for (std::size_t length : {55U, 56U, 57U, 63U, 64U, 65U, 119U, 120U, 127U, 128U, 129U}) {
    const std::string sample(length, 'x');
    fcr::Sha256 hasher;
    for (const char character : sample) {
      hasher.update(&character, 1);
    }
    const std::string streamed = hasher.finish_hex();
    FCR_CHECK_EQ(streamed, fcr::sha256_hex(sample));
    FCR_CHECK_EQ(streamed.size(), fcr::Sha256::kDigestHexDigits);
  }
}

FCR_TEST(digest, one_byte_difference_changes_the_digest) {
  std::string first(100, 'a');
  std::string second = first;
  second[50] = 'b';
  FCR_CHECK_NE(fcr::sha256_hex(first), fcr::sha256_hex(second));
}

FCR_TEST(digest, empty_input_through_the_pointer_overload) {
  FCR_CHECK_EQ(fcr::sha256_hex(nullptr, 0), fcr::sha256_hex(""));
}
