// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The canonical state codec: exact round-tripping through a real store, the
// strictness of the decoder, and the fact that a state document is a complete
// description of the authoritative state.

#include <string>
#include <vector>

#include "detail/codec.hpp"
#include "dccp/facility_capacity_reservation/digest.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

const std::vector<fcr_test::PoolSpec> kPools = {
    {fcr::ResourceKind::Rack, "hall-a", 40, 4, 6},
    {fcr::ResourceKind::Power, "feed-a", 400'000, 0, 10'000},
};

/// A populated durable store whose committed state document a test can inspect.
class PopulatedStore {
 public:
  explicit PopulatedStore(const std::string& label)
      : directory_(label), root_(directory_ / "store"), snapshot_(make_snapshot()) {
    fcr::Result<fcr::Store> created = fcr::Store::create(root_);
    FCR_REQUIRE(created.has_value());
    store_ = std::move(created).value();
    populate();
    // The identity and the authority epoch are captured while the store is open:
    // after close() every accessor reports STORE_CLOSED, which is itself part of
    // the contract.
    incarnation_ = store_.incarnation().value();
    epoch_ = store_.epoch().value();
  }

  ~PopulatedStore() { store_.close(); }

  PopulatedStore(const PopulatedStore&) = delete;
  PopulatedStore& operator=(const PopulatedStore&) = delete;

  const std::filesystem::path& root() const noexcept { return root_; }
  const fcr::Incarnation& incarnation() const noexcept { return incarnation_; }
  fcr::AuthorityEpoch epoch() const noexcept { return epoch_; }
  fcr::Store& store() noexcept { return store_; }

  /// The committed state document, read from disk.
  std::string committed_document() const {
    const std::filesystem::path path = fcr_test::committed_state_file(root_);
    FCR_REQUIRE(fcr_test::file_exists(path));
    return fcr_test::read_text_file(path);
  }

  /// Decodes a document against this store's identity.
  fcr::Result<fcr::ReservationLedger> decode(
      const std::string& document, const fcr::detail::StateLimits& limits = fcr::detail::StateLimits{}) {
    return fcr::detail::decode_state(document, incarnation_, epoch_, fcr::LedgerOptions{},
                                     fcr::CapacityOrigin::Restored, limits);
  }

 private:
  static fcr::CapacitySnapshot make_snapshot() {
    return fcr_test::make_snapshot("snap-1", "site-1", 3, 1'000, kPools);
  }

  void populate() {
    FCR_REQUIRE_OK(store_.install_capacity(snapshot_, fcr::Tick(1'000)));
    fcr_test::Fixture fixture;
    fixture.snapshot = snapshot_;
    fixture.epoch = store_.epoch().value();

    FCR_REQUIRE_OK(store_.reserve(fcr_test::reserve_request(
        fixture, "res-a", "attempt-1",
        {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 10),
         fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 50'000)},
        1'100, 900'000)));
    const fcr::Result<fcr::ReserveOutcome> created = store_.reserve(fcr_test::reserve_request(
        fixture, "res-b", "attempt-2", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 5)}, 1'100, 900'000));
    FCR_REQUIRE(created.has_value());
    FCR_REQUIRE_OK(store_.amend(fcr_test::amend_request(
        fixture, "res-b", "attempt-3", created->reservation.record.generation,
        {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 7)}, 1'100, 950'000)));
    FCR_REQUIRE_OK(
        store_.release(fcr_test::release_request(fixture, "res-a", "attempt-4", fcr::ReservationGeneration(1))));
  }

  fcr_test::TempDir directory_;
  std::filesystem::path root_;
  fcr::CapacitySnapshot snapshot_;
  fcr::Store store_;
  fcr::Incarnation incarnation_;
  fcr::AuthorityEpoch epoch_;
};

/// A digest over everything an observer can see, used to prove that a decoded
/// state is the same state.
std::string state_fingerprint(const fcr::Store& store) {
  std::string material;
  const fcr::Result<std::vector<fcr::ReservationView>> views = store.list();
  if (!views.has_value()) {
    return "error";
  }
  for (const fcr::ReservationView& view : *views) {
    const fcr::ReservationRecord& record = view.record;
    material.append(record.id.value());
    material.push_back('|');
    material.append(fcr::format_unsigned(record.generation.value()));
    material.push_back('|');
    material.append(fcr::format_unsigned(record.revision.value()));
    material.push_back('|');
    material.append(record.last_attempt.value());
    material.push_back('|');
    material.append(record.claimant.value());
    material.push_back('|');
    material.append(record.priority.value());
    material.push_back('|');
    material.append(fcr::headroom_class_token(record.headroom));
    material.push_back('|');
    material.append(fcr::reservation_state_token(record.state));
    material.push_back('|');
    material.append(fcr::format_unsigned(record.validity.start.value()));
    material.push_back('|');
    material.append(fcr::format_unsigned(record.validity.deadline.value()));
    material.push_back('|');
    for (const fcr::ResourceClaim& claim : record.claims) {
      material.append(claim.pool.to_string());
      material.push_back(':');
      material.append(fcr::format_unsigned(claim.amount));
      material.push_back(',');
    }
    material.push_back('|');
    for (const fcr::AmendmentEntry& entry : record.lineage) {
      material.append(fcr::format_unsigned(entry.generation.value()));
      material.push_back(':');
      material.append(fcr::format_unsigned(entry.predecessor.value()));
      material.push_back(':');
      material.append(entry.attempt.value());
      material.push_back(':');
      material.append(fcr::amendment_cause_token(entry.cause));
      material.push_back(':');
      material.append(entry.detail);
      material.push_back(',');
    }
    material.push_back('|');
    if (record.termination.has_value()) {
      material.append(fcr::transition_cause_token(record.termination->cause));
      material.push_back(':');
      material.append(record.termination->actor.value());
      material.push_back(':');
      material.append(record.termination->detail);
    }
    material.push_back(';');
  }
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = store.pools();
  if (pools.has_value()) {
    for (const fcr::PoolAccount& account : *pools) {
      material.append(account.pool.to_string());
      material.push_back(':');
      material.append(fcr::format_unsigned(account.committed));
      material.push_back(':');
      material.append(fcr::format_unsigned(account.protected_));
      material.push_back(':');
      material.append(fcr::format_unsigned(account.free));
      material.push_back(';');
    }
  }
  const fcr::Result<fcr::Revision> revision = store.revision();
  if (revision.has_value()) {
    material.append(fcr::format_unsigned(revision->value()));
  }
  return fcr::sha256_hex(material);
}

/// Reseals a state body so a test can reach past the digest check.
///
/// The digest covers the body only, so it is computed before the digest line is
/// appended.
std::string reseal(std::string body) {
  const std::string digest = fcr::sha256_hex(body);
  body.append("digest=sha256:");
  body.append(digest);
  body.push_back('\n');
  return body;
}

std::string body_of(const std::string& document) {
  const std::size_t position = document.rfind("digest=sha256:");
  return document.substr(0, position);
}

}  // namespace

FCR_TEST(codec, a_populated_store_round_trips_through_its_document) {
  PopulatedStore fixture("codec_roundtrip");
  const std::string original = state_fingerprint(fixture.store());
  const std::string document = fixture.committed_document();
  FCR_CHECK(fcr::detail::looks_like_state(document));

  fixture.store().close();
  fcr::Result<fcr::Store> reopened = fcr::Store::open(fixture.root());
  FCR_REQUIRE(reopened.has_value());
  FCR_CHECK_EQ(state_fingerprint(*reopened), original);

  // The document the reopened store commits is byte-identical to the one it
  // read, which is what makes the format canonical rather than merely readable.
  const std::string again = fcr_test::read_text_file(fcr_test::committed_state_file(fixture.root()));
  FCR_CHECK_EQ(again, document);
  reopened->close();
}

FCR_TEST(codec, the_document_holds_every_field_of_the_state) {
  PopulatedStore fixture("codec_fields");
  const std::string document = fixture.committed_document();
  for (const char* key :
       {"incarnation=", "revision=", "last-tick=", "capacity=1", "snapshot-ref=snap-1",
        "snapshot-source-generation=3", "snapshot-facility=site-1", "pools=2", "pool=rack|hall-a|40|4|6",
        "reservations=2", "reservation.begin", "reservation.id=res-a", "reservation.generation=1",
        "reservation.claimant=claimant-a", "reservation.headroom=guaranteed", "reservation.validity=1100|900000",
        "reservation.state=released", "claim=power|feed-a|50000", "reservation.lineage=1", "lineage=",
        "termination=", "attempts=4", "attempt=", "end\n", "digest=sha256:"}) {
    FCR_CHECK(document.find(key) != std::string::npos);
  }
}

FCR_TEST(codec, the_decoder_refuses_a_tampered_body) {
  PopulatedStore fixture("codec_tamper");
  std::string tampered = fixture.committed_document();
  fcr_test::patch_text(tampered, "reservation.claimant=claimant-a", "reservation.claimant=claimant-b");
  FCR_CHECK_ERROR(fixture.decode(tampered), fcr::ErrorCode::DigestMismatch);
}

FCR_TEST(codec, the_decoder_refuses_an_out_of_place_key) {
  PopulatedStore fixture("codec_keys");
  const std::string document = fixture.committed_document();

  std::string unknown = body_of(document);
  fcr_test::patch_text(unknown, "revision=", "reviision=");
  FCR_CHECK_ERROR(fixture.decode(reseal(unknown)), fcr::ErrorCode::MalformedRecord);

  // The decoder reads keys positionally, so a key that is repeated out of place
  // is reported as the first key that does not belong where it was found. That
  // is stricter than a duplicate-key scan: nothing is resolved by first match.
  std::string reordered = body_of(document);
  fcr_test::patch_text(reordered, "reservation.claimant=claimant-a",
                       "reservation.tenant=tenant-a\nreservation.claimant=claimant-a");
  FCR_CHECK_ERROR(fixture.decode(reseal(reordered)), fcr::ErrorCode::MalformedRecord);

  // A repetition of the key the decoder expects next is refused by the framing
  // rather than accepted as a second value.
  std::string repeated = body_of(document);
  fcr_test::patch_text(repeated, "reservation.generation=",
                       "reservation.id=res-duplicate\nreservation.generation=");
  FCR_CHECK_ERROR(fixture.decode(reseal(repeated)), fcr::ErrorCode::MalformedRecord);
}

FCR_TEST(codec, the_decoder_refuses_a_wrong_incarnation) {
  PopulatedStore fixture("codec_incarnation");
  const fcr::Incarnation other = fcr::Incarnation::generate();
  FCR_CHECK_ERROR(fcr::detail::decode_state(fixture.committed_document(), other, fixture.epoch(),
                                            fcr::LedgerOptions{}, fcr::CapacityOrigin::Restored,
                                            fcr::detail::StateLimits{}),
                  fcr::ErrorCode::IncarnationMismatch);
}

FCR_TEST(codec, the_decoder_refuses_a_wrong_format_version) {
  PopulatedStore fixture("codec_version");
  std::string body = body_of(fixture.committed_document());
  fcr_test::patch_text(body, "fcr-state 1", "fcr-state 7");
  FCR_CHECK_ERROR(fixture.decode(reseal(body)), fcr::ErrorCode::UnsupportedFormatVersion);
}

FCR_TEST(codec, the_decoder_refuses_a_miscount_or_a_missing_terminator) {
  PopulatedStore fixture("codec_counts");
  const std::string document = fixture.committed_document();

  std::string miscount = body_of(document);
  fcr_test::patch_text(miscount, "reservations=2", "reservations=1");
  FCR_CHECK_ERROR(fixture.decode(reseal(miscount)), fcr::ErrorCode::MalformedRecord);

  std::string unterminated = body_of(document);
  fcr_test::patch_text(unterminated, "reservation.end\n", "");
  FCR_CHECK_ERROR(fixture.decode(reseal(unterminated)), fcr::ErrorCode::MalformedRecord);
}

FCR_TEST(codec, the_decoder_refuses_an_out_of_order_reservation) {
  PopulatedStore fixture("codec_order");
  std::string body = body_of(fixture.committed_document());
  // "res-a" sorts before "res-b"; renaming it to "res-z" puts the pair out of
  // canonical order without changing any field's shape.
  fcr_test::patch_text(body, "reservation.id=res-a", "reservation.id=res-z");
  FCR_CHECK_ERROR(fixture.decode(reseal(body)), fcr::ErrorCode::NonCanonicalOrder);
}

FCR_TEST(codec, the_decoder_refuses_an_unsorted_claim_set) {
  PopulatedStore fixture("codec_claims");
  std::string body = body_of(fixture.committed_document());
  // The rack claim sorts before the power claim; reversing them makes the state
  // non-canonical even though every value is still well formed.
  fcr_test::patch_text(body, "claim=rack|hall-a|10\nclaim=power|feed-a|50000\n",
                       "claim=power|feed-a|50000\nclaim=rack|hall-a|10\n");
  FCR_CHECK_ERROR(fixture.decode(reseal(body)), fcr::ErrorCode::NonCanonicalOrder);
}

FCR_TEST(codec, the_decoder_refuses_a_declared_bound_that_is_too_small) {
  PopulatedStore fixture("codec_bounds");
  const std::string document = fixture.committed_document();

  fcr::detail::StateLimits reservations;
  reservations.max_reservations = 1;
  FCR_CHECK_ERROR(fixture.decode(document, reservations), fcr::ErrorCode::LimitExceeded);

  fcr::detail::StateLimits lines;
  lines.max_lines = 5;
  FCR_CHECK_ERROR(fixture.decode(document, lines), fcr::ErrorCode::LimitExceeded);

  fcr::detail::StateLimits bytes;
  bytes.max_bytes = 16;
  FCR_CHECK_ERROR(fixture.decode(document, bytes), fcr::ErrorCode::LimitExceeded);

  fcr::detail::StateLimits attempts;
  attempts.max_attempts = 2;
  FCR_CHECK_ERROR(fixture.decode(document, attempts), fcr::ErrorCode::LimitExceeded);
}

FCR_TEST(codec, the_decoder_refuses_a_zero_generation_or_a_degenerate_interval) {
  PopulatedStore fixture("codec_values");
  const std::string document = fixture.committed_document();

  std::string zero_generation = body_of(document);
  fcr_test::patch_text(zero_generation, "reservation.generation=1", "reservation.generation=0");
  FCR_CHECK_ERROR(fixture.decode(reseal(zero_generation)), fcr::ErrorCode::MissingField);

  std::string degenerate = body_of(document);
  fcr_test::patch_text(degenerate, "reservation.validity=1100|900000", "reservation.validity=1100|1100");
  FCR_CHECK_ERROR(fixture.decode(reseal(degenerate)), fcr::ErrorCode::InvalidInterval);
}

FCR_TEST(codec, looks_like_state_is_a_cheap_prefix_test) {
  FCR_CHECK(fcr::detail::looks_like_state("fcr-state 1\n"));
  FCR_CHECK(!fcr::detail::looks_like_state("fcr-head 1\n"));
  FCR_CHECK(!fcr::detail::looks_like_state(""));
  FCR_CHECK(!fcr::detail::looks_like_state("fcr-state"));
}

FCR_TEST(codec, a_state_document_is_not_a_head_or_meta_document) {
  fcr_test::TempDir directory("codec_kinds");
  const std::filesystem::path root = directory / "store";
  fcr::Result<fcr::Store> created = fcr::Store::create(root);
  FCR_REQUIRE(created.has_value());
  FCR_REQUIRE_OK(
      created->install_capacity(fcr_test::make_snapshot("snap-1", "site-1", 3, 1'000, kPools), fcr::Tick(1'000)));
  FCR_CHECK(fcr::detail::looks_like_state(fcr_test::read_text_file(fcr_test::committed_state_file(root))));
  FCR_CHECK(!fcr::detail::looks_like_state(fcr_test::read_text_file(root / "fcr.head")));
  FCR_CHECK(!fcr::detail::looks_like_state(fcr_test::read_text_file(root / "fcr.meta")));
  created->close();
}

FCR_TEST(codec, the_decoder_refuses_a_duplicated_attempt_identity) {
  PopulatedStore fixture("codec_attempts");
  std::string body = body_of(fixture.committed_document());

  // Adding an attempt line without changing the declared count breaks the
  // framing, which is refused before any attempt is parsed. The search anchors on
  // the start of a line because "attempt=" also occurs inside
  // "reservation.last-attempt=".
  const std::size_t first_attempt = body.find("\nattempt=");
  FCR_REQUIRE(first_attempt != std::string::npos);
  const std::size_t first_start = first_attempt + 1;
  const std::size_t first_end = body.find('\n', first_start);
  FCR_REQUIRE(first_end != std::string::npos);
  std::string extra = body.substr(first_start, first_end - first_start);
  const std::size_t separator = extra.find('|');
  FCR_REQUIRE(separator != std::string::npos);
  extra.replace(separator, std::string("|reserve|res-a|").size(), "|reserve|res-z|");

  // Adding a line inside the attempt block without changing the declared count
  // breaks the framing: the decoder reads the declared number of attempts and
  // then expects the terminator, which is not where it is.
  const std::size_t terminator = body.rfind("end\n");
  FCR_REQUIRE(terminator != std::string::npos);
  std::string unframed = body;
  unframed.insert(terminator, extra + "\n");
  FCR_CHECK_ERROR(fixture.decode(reseal(unframed)), fcr::ErrorCode::MalformedRecord);

  // Keeping the declared count consistent reaches the duplicate identity check.
  const std::size_t attempts_line = body.find("attempts=");
  FCR_REQUIRE(attempts_line != std::string::npos);
  const std::size_t attempts_end = body.find('\n', attempts_line);
  std::string counted = body;
  counted.replace(attempts_line, attempts_end - attempts_line, "attempts=5");
  counted.insert(attempts_end + 1, extra + "\n");
  FCR_CHECK_ERROR(fixture.decode(reseal(counted)), fcr::ErrorCode::DuplicateField);
}

FCR_TEST(codec, a_hand_built_ledger_round_trips_through_encode_and_decode) {
  // The codec is exercised through its own surface as well as through the store,
  // so a defect in the store's publication path cannot mask a defect in the
  // encoding.
  const fcr::CapacitySnapshot snapshot = fcr_test::make_snapshot("snap-1", "site-1", 3, 1'000, kPools);
  const fcr::Incarnation incarnation = fcr::Incarnation::generate();
  fcr::Result<fcr::ReservationLedger> ledger =
      fcr::ReservationLedger::create(incarnation, fcr::AuthorityEpoch(1), fcr::LedgerOptions{});
  FCR_REQUIRE(ledger.has_value());
  FCR_REQUIRE_OK(ledger->install_capacity(snapshot, fcr::CapacityOrigin::Consumed));

  fcr_test::Fixture fixture;
  fixture.snapshot = snapshot;
  fixture.epoch = ledger->epoch();
  FCR_REQUIRE_OK(ledger->reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 3)}, 1'100, 900'000)));

  const fcr::Result<std::string> document = fcr::detail::encode_state(*ledger);
  FCR_REQUIRE(document.has_value());

  const fcr::Result<fcr::ReservationLedger> decoded =
      fcr::detail::decode_state(*document, incarnation, ledger->epoch(), fcr::LedgerOptions{},
                                fcr::CapacityOrigin::Consumed, fcr::detail::StateLimits{});
  FCR_REQUIRE(decoded.has_value());
  FCR_CHECK_EQ(decoded->revision().value(), ledger->revision().value());
  FCR_CHECK_EQ(decoded->reservation_count(), std::size_t{1});
  FCR_CHECK_EQ(decoded->last_observed_tick().value(), ledger->last_observed_tick().value());

  const fcr::Result<std::string> again = fcr::detail::encode_state(*decoded);
  FCR_REQUIRE(again.has_value());
  FCR_CHECK_EQ(*again, *document);
  const fcr::Result<fcr::VerificationReport> report = decoded->verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
}
