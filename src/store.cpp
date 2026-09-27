// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity_reservation/store.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "detail/codec.hpp"
#include "detail/fault.hpp"
#include "dccp/facility_capacity_reservation/digest.hpp"
#include "dccp/facility_capacity_reservation/text.hpp"
#include "file_ops.hpp"

namespace dccp::facility_capacity_reservation {
namespace {

constexpr std::string_view kMetaSignature = "fcr-store";
constexpr std::string_view kHeadSignature = "fcr-head";
constexpr std::string_view kDigestPrefix = "digest=sha256:";
constexpr std::string_view kLockFileName = "fcr.lock";
constexpr std::string_view kMetaFileName = "fcr.meta";
constexpr std::string_view kHeadFileName = "fcr.head";
constexpr std::string_view kGenerationsDirectory = "generations";
constexpr std::string_view kStateFilePrefix = "state-";
constexpr std::string_view kStateFileSuffix = ".fcr";

/// Hexadecimal digest characters embedded in a generation file name. The full
/// digest lives in the head pointer; the name carries a prefix so two
/// generations of the same revision cannot collide on disk.
constexpr std::size_t kStateFileNameDigestChars = 16;

/// Appends the integrity digest line covering everything written so far.
///
/// The digest is computed over the body before the digest line is appended, so
/// the verification path — which hashes every byte before the final line — sees
/// exactly the bytes that were hashed here.
std::string seal(std::string body) {
  const std::string digest = sha256_hex(body);
  body.append(kDigestPrefix);
  body.append(digest);
  body.push_back('\n');
  return body;
}

Result<void> verify_seal(std::string_view document, std::string_view what) {
  if (document.size() < kDigestPrefix.size() + 1) {
    return Error(ErrorCode::TruncatedInput, std::string(what) + " is too short to be a sealed document");
  }
  FCR_TRY(lines, split_lines(document, 64));
  const std::string_view digest_line = lines.back();
  if (digest_line.size() != kDigestPrefix.size() + Sha256::kDigestHexDigits ||
      digest_line.substr(0, kDigestPrefix.size()) != kDigestPrefix) {
    return Error(ErrorCode::MalformedRecord, std::string(what) + " does not end with an integrity digest");
  }
  const std::string_view declared = digest_line.substr(kDigestPrefix.size());
  if (!is_lower_hex(declared, Sha256::kDigestHexDigits)) {
    return Error(ErrorCode::MalformedRecord, std::string(what) + " digest is not lowercase hexadecimal");
  }
  const std::size_t body_bytes = document.size() - digest_line.size() - 1;
  if (sha256_hex(document.substr(0, body_bytes)) != declared) {
    return Error(ErrorCode::DigestMismatch, std::string(what) + " digest does not match its body");
  }
  return ok();
}

/// Strict reader for the two sealed key/value documents the store keeps beside
/// the state: the store identity and the head pointer.
class SealedDocument {
 public:
  static Result<SealedDocument> parse(std::string_view document, std::string_view signature, std::string_view what) {
    FCR_TRYV(verify_seal(document, what));
    FCR_TRY(lines, split_lines(document, 64));
    if (lines.size() < 3) {
      return Error(ErrorCode::TruncatedInput, std::string(what) + " is too short");
    }
    const std::string prefix = std::string(signature) + " ";
    if (lines[0].size() <= prefix.size() || lines[0].substr(0, prefix.size()) != prefix) {
      return Error(ErrorCode::MalformedRecord, std::string(what) + " does not start with its signature");
    }
    FCR_TRY(version, parse_unsigned(lines[0].substr(prefix.size())));
    if (version != kStateFormatVersion) {
      return Error(ErrorCode::UnsupportedFormatVersion,
                   std::string(what) + " format version " + format_unsigned(version) + " is not supported");
    }
    SealedDocument parsed;
    parsed.lines_.assign(lines.begin() + 1, lines.end() - 1);
    return parsed;
  }

  /// Every key must appear exactly once: a repeated key is refused rather than
  /// resolved by first match.
  Result<std::string_view> require(std::string_view key) const {
    std::string prefix(key);
    prefix.push_back('=');
    std::string_view found;
    bool seen = false;
    for (const std::string_view line : lines_) {
      if (line.size() >= prefix.size() && line.substr(0, prefix.size()) == prefix) {
        if (seen) {
          return Error(ErrorCode::DuplicateField, "the document repeats the key \"" + std::string(key) + "\"");
        }
        seen = true;
        found = line.substr(prefix.size());
      }
    }
    if (!seen) {
      return Error(ErrorCode::MissingField, "the document is missing the key \"" + std::string(key) + "\"");
    }
    return found;
  }

  Result<std::uint64_t> require_number(std::string_view key) const {
    FCR_TRY(text, require(key));
    return parse_unsigned(text);
  }

 private:
  std::vector<std::string_view> lines_;
};

bool matches_state_file_pattern(std::string_view name) noexcept {
  if (!file_ops::is_safe_file_name(name)) {
    return false;
  }
  const std::size_t minimum = kStateFilePrefix.size() + 1 + kStateFileNameDigestChars + kStateFileSuffix.size();
  if (name.size() < minimum) {
    return false;
  }
  if (name.substr(0, kStateFilePrefix.size()) != kStateFilePrefix) {
    return false;
  }
  if (name.substr(name.size() - kStateFileSuffix.size()) != kStateFileSuffix) {
    return false;
  }
  const std::string_view middle =
      name.substr(kStateFilePrefix.size(), name.size() - kStateFilePrefix.size() - kStateFileSuffix.size());
  const std::size_t separator = middle.find('-');
  if (separator == std::string_view::npos) {
    return false;
  }
  const std::string_view revision = middle.substr(0, separator);
  const std::string_view digest = middle.substr(separator + 1);
  return is_canonical_unsigned(revision) && is_lower_hex(digest, kStateFileNameDigestChars);
}

std::string state_file_name_for(Revision revision, const std::string& document_digest) {
  std::string name(kStateFilePrefix);
  name.append(format_unsigned(revision.value()));
  name.push_back('-');
  name.append(document_digest.substr(0, kStateFileNameDigestChars));
  name.append(kStateFileSuffix);
  return name;
}

/// Builds the sealed head pointer document. This is the single commit point of
/// the publication protocol: until it is replaced, the previous state is current.
std::string build_head(const Incarnation& incarnation, std::uint64_t sequence, Revision revision,
                       AuthorityEpoch epoch, std::string_view state_file, std::string_view state_digest,
                       std::uint64_t state_bytes) {
  std::string head;
  head.append(kHeadSignature);
  head.push_back(' ');
  head.append(format_unsigned(kStateFormatVersion));
  head.push_back('\n');
  head.append("incarnation=");
  head.append(incarnation.to_string());
  head.push_back('\n');
  head.append("head-sequence=");
  head.append(format_unsigned(sequence));
  head.push_back('\n');
  head.append("revision=");
  head.append(format_unsigned(revision.value()));
  head.push_back('\n');
  head.append("epoch=");
  head.append(format_unsigned(epoch.value()));
  head.push_back('\n');
  head.append("state-file=");
  head.append(state_file);
  head.push_back('\n');
  head.append("state-digest=");
  head.append(state_digest);
  head.push_back('\n');
  head.append("state-bytes=");
  head.append(format_unsigned(state_bytes));
  head.push_back('\n');
  return seal(std::move(head));
}

struct ParsedHead {
  Incarnation incarnation;
  std::uint64_t sequence = 0;
  Revision revision;
  AuthorityEpoch epoch;
  std::string state_file;
  std::string state_digest;
  std::uint64_t state_bytes = 0;
};

Result<ParsedHead> parse_head(std::string_view document) {
  FCR_TRY(parsed, SealedDocument::parse(document, kHeadSignature, "head pointer"));
  ParsedHead head;
  FCR_TRY(incarnation_text, parsed.require("incarnation"));
  FCR_TRY(incarnation, Incarnation::parse(incarnation_text));
  head.incarnation = incarnation;
  FCR_TRY(sequence, parsed.require_number("head-sequence"));
  if (sequence == 0) {
    return Error(ErrorCode::HeadCorrupt, "the head pointer declares a zero head sequence");
  }
  head.sequence = sequence;
  FCR_TRY(revision, parsed.require_number("revision"));
  head.revision = Revision(revision);
  FCR_TRY(epoch, parsed.require_number("epoch"));
  if (epoch == 0) {
    return Error(ErrorCode::HeadCorrupt, "the head pointer declares a zero writer authority epoch");
  }
  head.epoch = AuthorityEpoch(epoch);
  FCR_TRY(state_file, parsed.require("state-file"));
  if (!matches_state_file_pattern(state_file)) {
    return Error(ErrorCode::PathEscapesRoot,
                 "the head pointer names a generation file that is not a safe single path component")
        .with_subject(sanitize_for_display(state_file));
  }
  head.state_file = std::string(state_file);
  FCR_TRY(state_digest, parsed.require("state-digest"));
  if (!is_lower_hex(state_digest, Sha256::kDigestHexDigits)) {
    return Error(ErrorCode::HeadCorrupt, "the head pointer's state digest is not lowercase hexadecimal");
  }
  head.state_digest = std::string(state_digest);
  FCR_TRY(state_bytes, parsed.require_number("state-bytes"));
  head.state_bytes = state_bytes;
  return head;
}

Result<Incarnation> parse_meta_incarnation(std::string_view document) {
  FCR_TRY(parsed, SealedDocument::parse(document, kMetaSignature, "store identity"));
  FCR_TRY(incarnation_text, parsed.require("incarnation"));
  return Incarnation::parse(incarnation_text);
}

}  // namespace

std::string_view durability_mode_token(DurabilityMode mode) noexcept {
  switch (mode) {
    case DurabilityMode::Durable:
      return "durable";
    case DurabilityMode::Volatile:
      return "volatile";
  }
  return "unknown";
}

struct Store::FileLockHolder {
  file_ops::FileLock lock;
};

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

Store::Store() noexcept : mutex_(std::make_unique<std::mutex>()) {}

Store::~Store() { close(); }

Store::Store(Store&& other) noexcept : mutex_(std::make_unique<std::mutex>()) { *this = std::move(other); }

Store& Store::operator=(Store&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  close();
  lock_holder_ = std::move(other.lock_holder_);
  if (other.mutex_ == nullptr) {
    other.mutex_ = std::make_unique<std::mutex>();
  }
  mutex_ = std::move(other.mutex_);
  other.mutex_ = std::make_unique<std::mutex>();
  ledger_ = std::move(other.ledger_);
  options_ = other.options_;
  root_ = std::move(other.root_);
  generations_ = std::move(other.generations_);
  state_file_name_ = std::move(other.state_file_name_);
  state_digest_ = std::move(other.state_digest_);
  state_bytes_ = other.state_bytes_;
  head_sequence_ = other.head_sequence_;
  recovery_ = other.recovery_;
  durable_ = other.durable_;
  read_only_ = other.read_only_;
  closed_ = other.closed_;
  other.closed_ = true;
  other.lock_holder_.reset();
  other.ledger_ = ReservationLedger();
  return *this;
}

void Store::close() noexcept {
  if (mutex_ == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> guard(*mutex_);
  closed_ = true;
  // Releasing the file lock is the only externally visible effect of closing.
  // The operating system releases it on process death as well, which is what
  // makes writer handover after a crash possible.
  lock_holder_.reset();
}

bool Store::closed() const noexcept {
  if (mutex_ == nullptr) {
    return true;
  }
  std::lock_guard<std::mutex> guard(*mutex_);
  return closed_;
}

bool Store::read_only() const noexcept {
  if (mutex_ == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> guard(*mutex_);
  return read_only_;
}

bool Store::durable() const noexcept {
  if (mutex_ == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> guard(*mutex_);
  return durable_;
}

const std::filesystem::path& Store::root() const noexcept { return root_; }

RecoveryReport Store::recovery() const {
  if (mutex_ == nullptr) {
    return RecoveryReport();
  }
  std::lock_guard<std::mutex> guard(*mutex_);
  return recovery_;
}

// ---------------------------------------------------------------------------
// Guards
// ---------------------------------------------------------------------------

Result<void> Store::require_open_locked() const {
  if (closed_) {
    return Error(ErrorCode::StoreClosed, "the store has been closed");
  }
  return ok();
}

Result<void> Store::require_writable_locked() const {
  FCR_TRYV(require_open_locked());
  if (read_only_) {
    return Error(ErrorCode::StoreReadOnly, "this store handle holds observation authority only");
  }
  return ok();
}

// ---------------------------------------------------------------------------
// Publication
// ---------------------------------------------------------------------------

void Store::retire_generations_locked(std::size_t& removed, std::size_t& failures) const {
  removed = 0;
  failures = 0;
  const Result<std::vector<std::string>> names = file_ops::list_directory(generations_);
  if (!names.has_value()) {
    ++failures;
    return;
  }
  if (names->size() > options_.limits.max_generation_files) {
    ++failures;
    return;
  }
  for (const std::string& name : *names) {
    if (name == state_file_name_) {
      continue;
    }
    if (!matches_state_file_pattern(name)) {
      // A file this store did not write is left alone: cleanup never removes
      // something it cannot identify, so an operator's file cannot be destroyed
      // by opening a store in a directory that also holds something else.
      continue;
    }
    const Result<bool> gone = file_ops::remove_file(generations_ / name);
    if (!gone.has_value()) {
      ++failures;
      continue;
    }
    if (*gone) {
      ++removed;
    }
  }
}

Result<void> Store::publish_locked(const ReservationLedger& ledger) {
  // A volatile store has no directory to publish into. Reaching here would mean a
  // caller had bypassed the durability gate, and writing a state generation into
  // the process's working directory is exactly the failure this guard prevents.
  if (!durable_) {
    return Error(ErrorCode::Unsupported, "a volatile store has no directory to publish into");
  }
  FCR_TRY(document, detail::encode_state(ledger));
  const std::string document_digest = sha256_hex(document);
  const std::string name = state_file_name_for(ledger.revision(), document_digest);

  // The generation file name is derived here and never read from disk, but it is
  // still validated before it is joined to a path: a name that could escape the
  // store directory must not be constructible at all.
  if (!matches_state_file_pattern(name)) {
    return Error(ErrorCode::PathInvalid, "the generation file name is not a safe single path component")
        .with_subject(name);
  }

  const std::filesystem::path path = generations_ / name;
  FCR_TRYV(file_ops::atomic_write_file(path, document));

  // Verify the published artifact from its final name, at least as strictly as
  // the open path verifies it.
  FCR_TRY(read_back, file_ops::read_file(path, options_.limits.max_state_bytes));
  if (read_back.size() != document.size() || sha256_hex(read_back) != document_digest) {
    return Error(ErrorCode::IntegrityFailure, "the published state generation does not match what was written")
        .with_subject(name);
  }

  detail::reach_publish_stage(detail::PublishStage::AfterStateWrite);

  const std::uint64_t next_sequence = head_sequence_ + 1;
  const std::string head = build_head(ledger.incarnation(), next_sequence, ledger.revision(), ledger.epoch(), name,
                                      document_digest, static_cast<std::uint64_t>(document.size()));

  detail::reach_publish_stage(detail::PublishStage::BeforeHeadCommit);
  FCR_TRYV(file_ops::atomic_write_file(root_ / std::string(kHeadFileName), head));
  detail::reach_publish_stage(detail::PublishStage::AfterHeadCommit);

  head_sequence_ = next_sequence;
  state_file_name_ = name;
  state_digest_ = document_digest;
  state_bytes_ = static_cast<std::uint64_t>(document.size());

  std::size_t removed = 0;
  std::size_t failures = 0;
  retire_generations_locked(removed, failures);
  recovery_.cleanup_failures += failures;
  return ok();
}

Result<void> Store::take_authority_locked() {
  FCR_TRY(next_epoch, ledger_.epoch().next());
  const AuthorityEpoch previous = ledger_.epoch();
  ledger_.set_epoch(next_epoch);

  // The handover is published by rewriting the head pointer: the current epoch
  // lives there, while the state document records the epoch that minted each
  // record. Nothing else changes, so the state generation is reused unchanged.
  const std::string head = build_head(ledger_.incarnation(), head_sequence_ + 1, ledger_.revision(), next_epoch,
                                      state_file_name_, state_digest_, state_bytes_);
  FCR_TRYV(file_ops::atomic_write_file(root_ / std::string(kHeadFileName), head));
  head_sequence_ += 1;

  recovery_.previous_epoch = previous;
  recovery_.current_epoch = next_epoch;
  recovery_.recovered = true;
  return ok();
}

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

Result<Store> Store::in_memory(const StoreOptions& options) {
  StoreOptions effective = options;
  effective.durability = DurabilityMode::Volatile;
  FCR_TRY(ledger, ReservationLedger::create(Incarnation::generate(), AuthorityEpoch(1), effective.ledger));
  Store store;
  store.ledger_ = std::move(ledger);
  store.options_ = effective;
  store.durable_ = false;
  store.closed_ = false;
  store.recovery_.current_epoch = AuthorityEpoch(1);
  return store;
}

Result<Store> Store::create(const std::filesystem::path& root, const StoreOptions& options) {
  if (root.empty()) {
    return Error(ErrorCode::PathInvalid, "a store root must be named");
  }
  if (options.durability != DurabilityMode::Durable) {
    return Error(ErrorCode::Unsupported,
                 "a volatile store has no directory; use Store::in_memory() instead of create()");
  }
  const StoreOptions& effective = options;

  if (file_ops::path_exists(root)) {
    if (!file_ops::path_is_directory(root)) {
      std::error_code status_error;
      const std::filesystem::file_status status = std::filesystem::symlink_status(root, status_error);
      if (!status_error && status.type() == std::filesystem::file_type::symlink) {
        return Error(ErrorCode::SymlinkRefused,
                     "the store root is a symbolic link; the link is not followed")
            .with_subject(root.string());
      }
      return Error(ErrorCode::PathInvalid, "the store root exists and is not a directory").with_subject(root.string());
    }
    FCR_TRY(empty, file_ops::directory_is_empty(root));
    if (!empty) {
      return Error(ErrorCode::StoreNotEmpty, "the store root already holds entries").with_subject(root.string());
    }
  }
  FCR_TRYV(file_ops::create_directories(root));
  FCR_TRYV(file_ops::create_directories(root / std::string(kGenerationsDirectory)));

  FCR_TRY(lock, file_ops::FileLock::acquire(root / std::string(kLockFileName), file_ops::LockMode::Exclusive));

  // Re-check under the lock: another process may have created the store between
  // the emptiness test and the lock.
  if (file_ops::path_exists(root / std::string(kMetaFileName))) {
    return Error(ErrorCode::StoreAlreadyExists, "a store already exists in this directory").with_subject(root.string());
  }

  const Incarnation incarnation = Incarnation::generate();

  std::string meta;
  meta.append(kMetaSignature);
  meta.push_back(' ');
  meta.append(format_unsigned(kStateFormatVersion));
  meta.push_back('\n');
  meta.append("incarnation=");
  meta.append(incarnation.to_string());
  meta.push_back('\n');
  FCR_TRYV(file_ops::atomic_write_file(root / std::string(kMetaFileName), seal(std::move(meta))));

  FCR_TRY(ledger, ReservationLedger::create(incarnation, AuthorityEpoch(1), effective.ledger));

  Store store;
  store.ledger_ = std::move(ledger);
  store.options_ = effective;
  store.root_ = root;
  store.generations_ = root / std::string(kGenerationsDirectory);
  store.head_sequence_ = 0;
  store.durable_ = true;
  store.recovery_.opened_existing = false;
  store.recovery_.current_epoch = AuthorityEpoch(1);
  store.recovery_.revision = Revision(0);
  store.lock_holder_ = std::make_unique<FileLockHolder>();
  store.lock_holder_->lock = std::move(lock);

  FCR_TRYV(store.publish_locked(store.ledger_));
  store.closed_ = false;
  return store;
}

Result<Store> Store::open(const std::filesystem::path& root, const StoreOptions& options) {
  return open_impl(root, options, /*read_only=*/false);
}

Result<Store> Store::open_read_only(const std::filesystem::path& root, const StoreOptions& options) {
  return open_impl(root, options, /*read_only=*/true);
}

Result<Store> Store::open_impl(const std::filesystem::path& root, const StoreOptions& options, bool read_only) {
  if (root.empty()) {
    return Error(ErrorCode::PathInvalid, "a store root must be named");
  }
  if (file_ops::path_exists(root) && !file_ops::path_is_directory(root)) {
    // A store reached through a symbolic link is refused rather than followed:
    // the path the operator gave is not the path the state would come from.
    std::error_code status_error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(root, status_error);
    if (!status_error && status.type() == std::filesystem::file_type::symlink) {
      return Error(ErrorCode::SymlinkRefused, "the store root is a symbolic link; the link is not followed")
          .with_subject(root.string());
    }
  }
  if (!file_ops::path_exists(root) || !file_ops::path_is_directory(root)) {
    return Error(ErrorCode::StoreNotFound, "no store directory exists at this path").with_subject(root.string());
  }
  const std::filesystem::path meta_path = root / std::string(kMetaFileName);
  if (!file_ops::path_is_regular_file(meta_path)) {
    return Error(ErrorCode::StoreNotFound, "the directory does not hold a store identity file")
        .with_subject(root.string());
  }

  const file_ops::LockMode mode = read_only ? file_ops::LockMode::Shared : file_ops::LockMode::Exclusive;
  FCR_TRY(lock, file_ops::FileLock::acquire(root / std::string(kLockFileName), mode));

  FCR_TRY(meta_text, file_ops::read_file(meta_path, options.limits.max_meta_bytes));
  FCR_TRY(incarnation, parse_meta_incarnation(meta_text));

  const std::filesystem::path head_path = root / std::string(kHeadFileName);
  if (!file_ops::path_is_regular_file(head_path)) {
    // A missing head pointer is not repairable by this library. Rebuilding it
    // from the newest generation file on disk would promote a state that was
    // never committed — a mutation whose caller was told it failed. The store is
    // therefore refused rather than guessed at.
    return Error(ErrorCode::HeadMissing,
                 "the store has no head pointer; the last committed state cannot be identified and this library "
                 "will not promote an uncommitted generation")
        .with_subject(head_path.string());
  }
  FCR_TRY(head_text, file_ops::read_file(head_path, options.limits.max_head_bytes));
  FCR_TRY(head, parse_head(head_text));
  if (head.incarnation != incarnation) {
    return Error(ErrorCode::IncarnationMismatch,
                 "the head pointer belongs to a different store incarnation than the identity file")
        .with_subject(head.incarnation.to_string());
  }

  const std::filesystem::path state_path = root / std::string(kGenerationsDirectory) / head.state_file;
  if (!file_ops::path_is_regular_file(state_path)) {
    return Error(ErrorCode::StoreCorrupt, "the state generation the head pointer names is missing")
        .with_subject(head.state_file);
  }
  FCR_TRY(document, file_ops::read_file(state_path, options.limits.max_state_bytes));
  if (document.size() != head.state_bytes) {
    return Error(ErrorCode::IntegrityFailure, "the committed state generation has the wrong size")
        .with_subject(head.state_file);
  }
  if (sha256_hex(document) != head.state_digest) {
    return Error(ErrorCode::IntegrityFailure, "the committed state generation does not match its recorded digest")
        .with_subject(head.state_file);
  }

  const CapacityOrigin origin =
      options.trust_persisted_capacity ? CapacityOrigin::Consumed : CapacityOrigin::Restored;

  detail::StateLimits limits;
  limits.max_bytes = options.limits.max_state_bytes;
  limits.max_lines = options.limits.max_state_lines;
  limits.max_reservations = options.ledger.max_reservations;
  limits.max_attempts = options.ledger.max_attempts;
  limits.max_pools = options.ledger.max_pools;
  limits.max_pools_per_reservation = options.ledger.limits.max_claims;
  limits.max_lineage_per_reservation = options.ledger.limits.max_lineage_entries;

  FCR_TRY(ledger, detail::decode_state(document, incarnation, head.epoch, options.ledger, origin, limits));
  if (ledger.revision() != head.revision) {
    return Error(ErrorCode::StoreCorrupt, "the committed state generation declares a different revision than the head")
        .with_subject(head.state_file);
  }

  Store store;
  store.ledger_ = std::move(ledger);
  store.options_ = options;
  store.root_ = root;
  store.generations_ = root / std::string(kGenerationsDirectory);
  store.head_sequence_ = head.sequence;
  store.state_file_name_ = head.state_file;
  store.state_digest_ = head.state_digest;
  store.state_bytes_ = head.state_bytes;
  store.durable_ = true;
  store.read_only_ = read_only;
  store.closed_ = false;
  store.recovery_.opened_existing = true;
  store.recovery_.revision = head.revision;
  store.recovery_.capacity_restored = store.ledger_.has_capacity();
  store.recovery_.previous_epoch = head.epoch;
  store.recovery_.current_epoch = head.epoch;
  store.lock_holder_ = std::make_unique<FileLockHolder>();
  store.lock_holder_->lock = std::move(lock);

  if (!read_only) {
    FCR_TRYV(store.take_authority_locked());
  }

  std::size_t removed = 0;
  std::size_t failures = 0;
  store.retire_generations_locked(removed, failures);
  store.recovery_.orphan_generations_removed = removed;
  store.recovery_.cleanup_failures = failures;
  return store;
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------
//
// The store applies each request to the scratch copy that mutate() already made.
// It calls the ledger's staged operations directly rather than the self-staging
// wrappers, so exactly one copy of the ledger is made per mutation instead of
// two; the strong guarantee is unchanged because the scratch copy is still what
// receives the change.

namespace {

template <class Outcome, class Stage>
Result<Outcome> apply_stage(ReservationLedger& target, Stage&& stage) {
  Outcome outcome;
  FCR_TRYV(stage(target, outcome));
  return Result<Outcome>(std::move(outcome));
}

}  // namespace

Result<void> Store::install_capacity(const CapacitySnapshot& snapshot, Tick now) {
  if (now.is_zero()) {
    return Error(ErrorCode::MissingField, "installing capacity evidence requires the logical tick it was consumed at");
  }
  std::lock_guard<std::mutex> guard(*mutex_);
  FCR_TRYV(require_writable_locked());
  ReservationLedger staged = ledger_;
  FCR_TRYV(staged.install_capacity(snapshot, CapacityOrigin::Consumed));
  staged.note_observed_tick(now);
  if (durable_ && staged.revision() != ledger_.revision()) {
    FCR_TRYV(publish_locked(staged));
  }
  ledger_ = std::move(staged);
  return ok();
}

Result<ReserveOutcome> Store::reserve(const ReserveRequest& request) {
  return mutate([&request](ReservationLedger& staged) {
    return apply_stage<ReserveOutcome>(staged, [&request](ReservationLedger& target, ReserveOutcome& outcome) {
      return ReservationLedger::stage_reserve(request, target, outcome);
    });
  });
}

Result<AmendOutcome> Store::amend(const AmendRequest& request) {
  return mutate([&request](ReservationLedger& staged) {
    return apply_stage<AmendOutcome>(staged, [&request](ReservationLedger& target, AmendOutcome& outcome) {
      return ReservationLedger::stage_amend(request, target, outcome);
    });
  });
}

Result<ReleaseOutcome> Store::release(const ReleaseRequest& request) {
  return mutate([&request](ReservationLedger& staged) {
    return apply_stage<ReleaseOutcome>(staged, [&request](ReservationLedger& target, ReleaseOutcome& outcome) {
      return ReservationLedger::stage_release(request, target, outcome);
    });
  });
}

Result<RevokeOutcome> Store::revoke(const RevokeRequest& request) {
  return mutate([&request](ReservationLedger& staged) {
    return apply_stage<RevokeOutcome>(staged, [&request](ReservationLedger& target, RevokeOutcome& outcome) {
      return ReservationLedger::stage_revoke(request, target, outcome);
    });
  });
}

Result<ExpireOutcome> Store::expire(const ExpireRequest& request) {
  return mutate([&request](ReservationLedger& staged) {
    return apply_stage<ExpireOutcome>(staged, [&request](ReservationLedger& target, ExpireOutcome& outcome) {
      return ReservationLedger::stage_expire(request, target, outcome);
    });
  });
}

Result<ReconcileOutcome> Store::reconcile(const ReconcileRequest& request) {
  return mutate([&request](ReservationLedger& staged) {
    return apply_stage<ReconcileOutcome>(staged, [&request](ReservationLedger& target, ReconcileOutcome& outcome) {
      return ReservationLedger::stage_reconcile(request, target, outcome);
    });
  });
}

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

Result<RevalidationReport> Store::revalidate(const RevalidateRequest& request) const {
  std::lock_guard<std::mutex> guard(*mutex_);
  FCR_TRYV(require_open_locked());
  return ledger_.revalidate(request);
}

Result<VerificationReport> Store::verify() const {
  std::lock_guard<std::mutex> guard(*mutex_);
  FCR_TRYV(require_open_locked());
  FCR_TRY(report, ledger_.verify());

  // A durable store re-reads its committed artifact and re-derives everything
  // from the bytes: inspection is never weaker than the open path.
  if (durable_) {
    if (state_file_name_.empty()) {
      return Error(ErrorCode::HeadMissing, "the store holds no committed state generation");
    }
    const std::filesystem::path path = generations_ / state_file_name_;
    FCR_TRY(document, file_ops::read_file(path, options_.limits.max_state_bytes));
    if (sha256_hex(document) != state_digest_) {
      return Error(ErrorCode::IntegrityFailure, "the committed state generation does not match its recorded digest")
          .with_subject(state_file_name_);
    }
    detail::StateLimits limits;
    limits.max_bytes = options_.limits.max_state_bytes;
    limits.max_lines = options_.limits.max_state_lines;
    limits.max_reservations = options_.ledger.max_reservations;
    limits.max_attempts = options_.ledger.max_attempts;
    limits.max_pools = options_.ledger.max_pools;
    limits.max_pools_per_reservation = options_.ledger.limits.max_claims;
    limits.max_lineage_per_reservation = options_.ledger.limits.max_lineage_entries;
    FCR_TRY(reloaded, detail::decode_state(document, ledger_.incarnation(), ledger_.epoch(), options_.ledger,
                                           ledger_.capacity_origin(), limits));
    FCR_TRY(disk_report, reloaded.verify());
    if (reloaded.revision() != ledger_.revision() || !disk_report.ok) {
      return Error(ErrorCode::AccountingMismatch,
                   "the committed state generation disagrees with the state held in memory")
          .with_subject(state_file_name_);
    }
  }
  return report;
}

Result<LedgerStatus> Store::status() const {
  std::lock_guard<std::mutex> guard(*mutex_);
  if (closed_) {
    return Error(ErrorCode::StoreClosed, "the store has been closed");
  }
  return ledger_.status(durable_, closed_);
}

Result<std::optional<ReservationView>> Store::find(const ReservationId& id) const {
  std::lock_guard<std::mutex> guard(*mutex_);
  FCR_TRYV(require_open_locked());
  return ledger_.find(id);
}

Result<std::vector<ReservationView>> Store::list() const {
  std::lock_guard<std::mutex> guard(*mutex_);
  FCR_TRYV(require_open_locked());
  return ledger_.list();
}

Result<std::vector<PoolAccount>> Store::pools() const {
  std::lock_guard<std::mutex> guard(*mutex_);
  FCR_TRYV(require_open_locked());
  return ledger_.pool_accounts();
}

Result<CapacitySplit> Store::accounting_of(const PoolKey& key) const {
  std::lock_guard<std::mutex> guard(*mutex_);
  FCR_TRYV(require_open_locked());
  return ledger_.accounting_of(key);
}

Result<CapacitySnapshot> Store::capacity() const {
  std::lock_guard<std::mutex> guard(*mutex_);
  FCR_TRYV(require_open_locked());
  if (!ledger_.has_capacity()) {
    return Error(ErrorCode::NoCapacityInstalled, "the store holds no capacity evidence");
  }
  return ledger_.capacity();
}

Result<Tick> Store::last_observed_tick() const {
  std::lock_guard<std::mutex> guard(*mutex_);
  FCR_TRYV(require_open_locked());
  return ledger_.last_observed_tick();
}

Result<AuthorityEpoch> Store::epoch() const {
  std::lock_guard<std::mutex> guard(*mutex_);
  FCR_TRYV(require_open_locked());
  return ledger_.epoch();
}

Result<Revision> Store::revision() const {
  std::lock_guard<std::mutex> guard(*mutex_);
  FCR_TRYV(require_open_locked());
  return ledger_.revision();
}

Result<Incarnation> Store::incarnation() const {
  std::lock_guard<std::mutex> guard(*mutex_);
  FCR_TRYV(require_open_locked());
  return ledger_.incarnation();
}

}  // namespace dccp::facility_capacity_reservation
