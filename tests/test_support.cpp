// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>

namespace fcr_test {
namespace {

std::atomic<std::uint64_t> g_temp_counter{0};

std::string unique_label(const std::string& label) {
  const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return "fcr_test_" + label + "_" + std::to_string(now) + "_" +
         std::to_string(g_temp_counter.fetch_add(1));
}

}  // namespace

TempDir::TempDir(const std::string& label) {
  std::error_code error;
  std::filesystem::path base = std::filesystem::temp_directory_path(error);
  if (error) {
    base = std::filesystem::current_path();
  }
  path_ = base / unique_label(label);
  std::filesystem::create_directories(path_, error);
}

TempDir::~TempDir() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

// ---------------------------------------------------------------------------
// Rng
// ---------------------------------------------------------------------------

namespace {

std::uint64_t splitmix64(std::uint64_t& state) noexcept {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

std::uint64_t rotl(std::uint64_t value, int count) noexcept {
  return (value << count) | (value >> (64 - count));
}

}  // namespace

Rng::Rng(std::uint64_t seed) noexcept {
  std::uint64_t state = seed == 0 ? 0x2545F4914F6CDD1DULL : seed;
  for (std::uint64_t& word : state_) {
    word = splitmix64(state);
  }
}

std::uint64_t Rng::next() noexcept {
  const std::uint64_t result = rotl(state_[1] * 5ULL, 7) * 9ULL;
  const std::uint64_t t = state_[1] << 17;
  state_[2] ^= state_[0];
  state_[3] ^= state_[1];
  state_[1] ^= state_[2];
  state_[0] ^= state_[3];
  state_[2] ^= t;
  state_[3] = rotl(state_[3], 45);
  return result;
}

std::uint64_t Rng::below(std::uint64_t bound) noexcept { return bound == 0 ? 0 : next() % bound; }

std::uint64_t Rng::between(std::uint64_t low, std::uint64_t high) noexcept {
  if (high <= low) {
    return low;
  }
  return low + below(high - low + 1);
}

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

fcr::ScopeRef scope(const char* text) { return fcr::ScopeRef::parse(text, "scope").value(); }
fcr::SnapshotRef snapshot_ref(const char* text) { return fcr::SnapshotRef::parse(text, "snapshot ref").value(); }
fcr::FacilityRef facility_ref(const char* text) { return fcr::FacilityRef::parse(text, "facility ref").value(); }
fcr::ClaimantRef claimant(const char* text) { return fcr::ClaimantRef::parse(text, "claimant").value(); }
fcr::ActorRef actor(const char* text) { return fcr::ActorRef::parse(text, "actor").value(); }
fcr::TenantRef tenant(const char* text) { return fcr::TenantRef::parse(text, "tenant").value(); }
fcr::ServiceRef service(const char* text) { return fcr::ServiceRef::parse(text, "service").value(); }
fcr::PriorityRef priority(const char* text) { return fcr::PriorityRef::parse(text, "priority").value(); }
fcr::PolicyRef policy(const char* text) { return fcr::PolicyRef::parse(text, "policy").value(); }
fcr::AttemptId attempt(const char* text) { return fcr::AttemptId::parse(text, "attempt").value(); }
fcr::ReservationId reservation_id(const char* text) {
  return fcr::ReservationId::parse(text, "reservation id").value();
}

fcr::CapacitySnapshot make_snapshot(const char* ref, const char* facility, std::uint64_t generation,
                                    std::uint64_t captured_tick, const std::vector<PoolSpec>& pools) {
  fcr::CapacitySnapshot snapshot;
  snapshot.ref = snapshot_ref(ref);
  snapshot.facility = facility_ref(facility);
  snapshot.source_generation = fcr::SourceGeneration(generation);
  snapshot.captured_at_tick = fcr::Tick(captured_tick);
  for (const PoolSpec& spec : pools) {
    fcr::CapacityPool pool;
    pool.key.kind = spec.kind;
    pool.key.scope = scope(spec.scope_name);
    pool.gross = spec.gross;
    pool.withdrawn = spec.withdrawn;
    pool.floor = spec.floor;
    snapshot.pools.push_back(pool);
  }
  std::sort(snapshot.pools.begin(), snapshot.pools.end(),
            [](const fcr::CapacityPool& lhs, const fcr::CapacityPool& rhs) { return lhs.key < rhs.key; });
  return snapshot;
}

fcr::ResourceClaim claim(fcr::ResourceKind kind, const char* scope_name, std::uint64_t amount) {
  fcr::ResourceClaim value;
  value.pool.kind = kind;
  value.pool.scope = scope(scope_name);
  value.amount = amount;
  return value;
}

Fixture Fixture::make(const std::vector<PoolSpec>& pools, std::uint64_t seed) {
  Fixture fixture;
  fcr::Result<fcr::Store> opened = fcr::Store::in_memory();
  FCR_REQUIRE(opened.has_value());
  fixture.store = std::move(opened).value();
  fixture.snapshot = make_snapshot("snap-fixture", "site-fixture", seed, 1'000, pools);
  FCR_REQUIRE_OK(fixture.store.install_capacity(fixture.snapshot, fcr::Tick(1'000)));
  const fcr::Result<fcr::AuthorityEpoch> epoch = fixture.store.epoch();
  FCR_REQUIRE(epoch.has_value());
  fixture.epoch = *epoch;
  return fixture;
}

fcr::ReserveRequest reserve_request(const Fixture& fixture, const char* id, const char* attempt_name,
                                    std::vector<fcr::ResourceClaim> claims, std::uint64_t start,
                                    std::uint64_t deadline) {
  fcr::ReserveRequest request;
  request.attempt = attempt(attempt_name);
  request.id = reservation_id(id);
  request.claimant = claimant("claimant-a");
  request.tenant = tenant("tenant-a");
  request.service = service("service-a");
  request.priority = priority("priority-normal");
  request.headroom = fcr::HeadroomClass::Guaranteed;
  request.authority.epoch = fixture.epoch;
  request.expected_source_generation = fixture.snapshot.source_generation;
  request.validity.start = fcr::Tick(start);
  request.validity.deadline = fcr::Tick(deadline);
  request.now = fcr::Tick(start);
  request.claims = std::move(claims);
  return request;
}

fcr::AmendRequest amend_request(const Fixture& fixture, const char* id, const char* attempt_name,
                                fcr::ReservationGeneration generation, std::vector<fcr::ResourceClaim> claims,
                                std::uint64_t start, std::uint64_t deadline) {
  fcr::AmendRequest request;
  request.attempt = attempt(attempt_name);
  request.id = reservation_id(id);
  request.expected_generation = generation;
  request.actor = actor("claimant-a");
  request.authority.epoch = fixture.epoch;
  request.expected_source_generation = fixture.snapshot.source_generation;
  request.validity.start = fcr::Tick(start);
  request.validity.deadline = fcr::Tick(deadline);
  request.now = fcr::Tick(start);
  request.claims = std::move(claims);
  return request;
}

fcr::ReleaseRequest release_request(const Fixture& fixture, const char* id, const char* attempt_name,
                                    fcr::ReservationGeneration generation) {
  fcr::ReleaseRequest request;
  request.attempt = attempt(attempt_name);
  request.id = reservation_id(id);
  request.expected_generation = generation;
  request.actor = actor("claimant-a");
  request.authority.epoch = fixture.epoch;
  request.now = fcr::Tick(5'000);
  return request;
}

fcr::RevokeRequest revoke_request(const Fixture& fixture, const char* id, const char* attempt_name,
                                  fcr::ReservationGeneration generation) {
  fcr::RevokeRequest request;
  request.attempt = attempt(attempt_name);
  request.id = reservation_id(id);
  request.expected_generation = generation;
  request.actor = actor("facility-authority");
  request.policy = policy("policy-reclaim");
  request.authority.epoch = fixture.epoch;
  request.now = fcr::Tick(5'000);
  return request;
}

fcr::ExpireRequest expire_request(const Fixture& fixture, const char* attempt_name, std::uint64_t now) {
  fcr::ExpireRequest request;
  request.attempt = attempt(attempt_name);
  request.authority.epoch = fixture.epoch;
  request.now = fcr::Tick(now);
  return request;
}

fcr::ReconcileRequest reconcile_request(const Fixture& fixture, const char* attempt_name,
                                        const fcr::CapacitySnapshot& snapshot,
                                        fcr::SourceGeneration expected_source_generation,
                                        fcr::ReconcileMode mode) {
  fcr::ReconcileRequest request;
  request.attempt = attempt(attempt_name);
  request.snapshot = snapshot;
  request.expected_source_generation = expected_source_generation;
  request.mode = mode;
  request.actor = actor("facility-authority");
  request.policy = policy("policy-withdrawal");
  request.authority.epoch = fixture.epoch;
  request.now = fcr::Tick(9'000);
  return request;
}

// ---------------------------------------------------------------------------
// Independent reference model
// ---------------------------------------------------------------------------

bool reference_accounting(const fcr::CapacitySnapshot& snapshot,
                          const std::vector<ReferenceCommitment>& commitments, std::vector<ReferencePool>& out,
                          std::string& why) {
  out.clear();
  std::map<fcr::PoolKey, std::size_t> index;
  for (const fcr::CapacityPool& pool : snapshot.pools) {
    if (pool.withdrawn > pool.gross) {
      why = "the snapshot declares more withdrawn than gross capacity";
      return false;
    }
    const std::uint64_t available = pool.gross - pool.withdrawn;
    if (pool.floor > available) {
      why = "the snapshot declares a floor larger than the available capacity";
      return false;
    }
    ReferencePool entry;
    entry.pool = pool.key;
    entry.reservable = available - pool.floor;
    entry.free_units = entry.reservable;
    index.emplace(pool.key, out.size());
    out.push_back(entry);
  }
  for (const ReferenceCommitment& commitment : commitments) {
    const auto position = index.find(commitment.pool);
    if (position == index.end()) {
      why = "a commitment names a pool the snapshot does not declare";
      return false;
    }
    ReferencePool& entry = out[position->second];
    if (!commitment.holds_capacity) {
      continue;
    }
    if (commitment.amount == 0) {
      why = "a commitment asks for zero units";
      return false;
    }
    if (commitment.amount > entry.free_units) {
      why = "the commitments do not fit inside the pool";
      return false;
    }
    entry.free_units -= commitment.amount;
    if (commitment.committed_bucket) {
      entry.committed += commitment.amount;
    } else {
      entry.protected_units += commitment.amount;
    }
    ++entry.active;
  }
  for (const ReferencePool& entry : out) {
    if (entry.committed + entry.protected_units + entry.free_units != entry.reservable) {
      why = "the reference model's own closure failed";
      return false;
    }
  }
  return true;
}

std::vector<ReferenceCommitment> reference_from_views(const std::vector<fcr::ReservationView>& views) {
  std::vector<ReferenceCommitment> commitments;
  for (const fcr::ReservationView& view : views) {
    const bool holds = fcr::reservation_state_holds_capacity(view.record.state);
    const bool committed_bucket = fcr::headroom_counts_as_committed(view.record.headroom);
    for (const fcr::ResourceClaim& item : view.record.claims) {
      ReferenceCommitment commitment;
      commitment.pool = item.pool;
      commitment.amount = item.amount;
      commitment.committed_bucket = committed_bucket;
      commitment.holds_capacity = holds;
      commitments.push_back(commitment);
    }
  }
  return commitments;
}

const fcr::PoolAccount* find_pool(const std::vector<fcr::PoolAccount>& accounts, fcr::ResourceKind kind,
                                  const char* scope_name) {
  const fcr::PoolKey key{kind, scope(scope_name)};
  for (const fcr::PoolAccount& account : accounts) {
    if (account.pool == key) {
      return &account;
    }
  }
  return nullptr;
}

void check_accounting_matches_reference(const fcr::Store& store, const fcr::CapacitySnapshot& snapshot,
                                        const std::vector<ReferenceCommitment>& commitments) {
  std::vector<ReferencePool> expected;
  std::string why;
  if (!reference_accounting(snapshot, commitments, expected, why)) {
    FCR_FAIL("the reference model rejected the commitments: " + why);
    throw TestAborted{};
  }
  const fcr::Result<std::vector<fcr::PoolAccount>> accounts = store.pools();
  FCR_REQUIRE(accounts.has_value());
  FCR_REQUIRE(accounts->size() == expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const fcr::PoolAccount& actual = (*accounts)[index];
    const ReferencePool& want = expected[index];
    if (actual.pool != want.pool) {
      FCR_FAIL("pool order differs from the reference model at index " + std::to_string(index));
      throw TestAborted{};
    }
    FCR_CHECK_EQ(actual.reservable, want.reservable);
    FCR_CHECK_EQ(actual.committed, want.committed);
    FCR_CHECK_EQ(actual.protected_, want.protected_units);
    FCR_CHECK_EQ(actual.free, want.free_units);
    FCR_CHECK_EQ(static_cast<std::uint64_t>(actual.active_reservations), static_cast<std::uint64_t>(want.active));
  }
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

std::uint64_t runtime_u64(std::uint64_t value) noexcept { return value; }

bool runtime_bool(bool value) noexcept { return value; }

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  FCR_REQUIRE(stream.good());
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

void write_text_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  FCR_REQUIRE(stream.good());
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  stream.close();
  FCR_REQUIRE(!stream.fail());
}

std::vector<std::string> list_names(const std::filesystem::path& directory) {
  std::vector<std::string> names;
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error) || error) {
    return names;
  }
  for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, error)) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

bool file_exists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error;
}

void patch_text(std::string& text, const std::string& needle, const std::string& replacement) {
  const std::size_t position = text.find(needle);
  if (position == std::string::npos) {
    FCR_FAIL("the text does not contain the substring to patch: " + needle);
    throw TestAborted{};
  }
  text.replace(position, needle.size(), replacement);
}

void patch_file(const std::filesystem::path& path, const std::string& needle, const std::string& replacement) {
  std::string content = read_text_file(path);
  if (content.find(needle) == std::string::npos) {
    FCR_FAIL("the file does not contain the text to patch: " + needle);
    throw TestAborted{};
  }
  patch_text(content, needle, replacement);
  write_text_file(path, content);
}

void append_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream stream(path, std::ios::binary | std::ios::app);
  FCR_REQUIRE(stream.good());
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  stream.close();
}

std::filesystem::path committed_state_file(const std::filesystem::path& store_root) {
  const std::string head = read_text_file(store_root / "fcr.head");
  const std::string prefix = "state-file=";
  const std::size_t position = head.find(prefix);
  FCR_REQUIRE(position != std::string::npos);
  const std::size_t end = head.find('\n', position);
  FCR_REQUIRE(end != std::string::npos);
  return store_root / "generations" / head.substr(position + prefix.size(), end - position - prefix.size());
}

void truncate_file(const std::filesystem::path& path, std::uintmax_t bytes) {
  std::error_code error;
  std::filesystem::resize_file(path, bytes, error);
  FCR_REQUIRE(!error);
}

}  // namespace fcr_test
