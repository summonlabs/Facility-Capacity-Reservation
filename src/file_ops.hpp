// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Platform file primitives used by the durable store. Private to the library.
//
// Everything here is written so that authoritative state only ever changes by an
// atomic replace, and so that a lock is never held while a callback runs.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_SRC_FILE_OPS_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_SRC_FILE_OPS_HPP

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_capacity_reservation/status.hpp"

namespace dccp::facility_capacity_reservation::file_ops {

enum class LockMode {
  Shared,
  Exclusive,
};

/// Advisory, cross-process file lock.
///
/// The lock is held for as long as the object lives and is released on
/// destruction, including when the holding process dies. Acquisition never
/// blocks: a lock held by another process is reported as STORE_LOCKED so that the
/// caller decides what to do instead of waiting inside the library.
class FileLock {
 public:
  FileLock() noexcept;
  ~FileLock();
  FileLock(FileLock&&) noexcept;
  FileLock& operator=(FileLock&&) noexcept;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  static Result<FileLock> acquire(const std::filesystem::path& file, LockMode mode);

  bool held() const noexcept;
  void release() noexcept;

 private:
  struct Handle;
  explicit FileLock(std::unique_ptr<Handle> handle) noexcept;

  std::unique_ptr<Handle> handle_;
};

/// Reads a whole file, refusing anything larger than `max_bytes`.
///
/// The size check uses the file's own reported size before allocating, and the
/// content is re-checked while reading, so a file that grows between the two
/// cannot force an unbounded allocation. Symbolic links and other non-regular
/// files are refused rather than followed.
Result<std::string> read_file(const std::filesystem::path& path, std::size_t max_bytes);

/// Reads a whole file, returning std::nullopt when it does not exist.
Result<std::optional<std::string>> read_file_if_exists(const std::filesystem::path& path, std::size_t max_bytes);

/// Atomically replaces `path` with `content`.
///
/// The bytes go to a uniquely named temporary file in the same directory, are
/// flushed to stable storage, verified by re-reading, and only then renamed over
/// the target. On success the target holds either the old content or the new
/// content, never a mixture.
Result<void> atomic_write_file(const std::filesystem::path& path, std::string_view content);

/// Flushes a directory entry so a completed rename is durable where the platform
/// exposes it.
Result<void> flush_directory(const std::filesystem::path& directory);

/// Creates `directory` and any missing parents.
Result<void> create_directories(const std::filesystem::path& directory);

/// True when `path` names an existing regular file. Symbolic links are not
/// followed: a link inside a store directory is treated as untrusted input.
bool path_is_regular_file(const std::filesystem::path& path) noexcept;

/// True when `path` names an existing directory, without following a link.
bool path_is_directory(const std::filesystem::path& path) noexcept;

bool path_exists(const std::filesystem::path& path) noexcept;

/// True when the directory holds no entries at all.
Result<bool> directory_is_empty(const std::filesystem::path& directory);

/// Removes a file, reporting whether it existed. A missing file is not an error,
/// so cleanup paths are idempotent.
Result<bool> remove_file(const std::filesystem::path& path);

/// Removes a directory and its contents. Refuses to follow a symbolic link.
Result<void> remove_tree(const std::filesystem::path& directory);

/// Renames within the same filesystem, replacing an existing target.
Result<void> rename_replace(const std::filesystem::path& from, const std::filesystem::path& to);

/// Directory entry names, sorted byte-wise so every caller observes the same
/// order.
Result<std::vector<std::string>> list_directory(const std::filesystem::path& directory);

/// Process identifier, used only to make temporary file names unique.
std::string process_id_token();

/// Monotonic counter making temporary file names unique within a process.
std::string next_sequence_token();

/// True when `name` is a single path component: non-empty, not "." or "..", and
/// free of directory separators, drive qualifiers, NUL and control bytes.
bool is_safe_file_name(std::string_view name) noexcept;

}  // namespace dccp::facility_capacity_reservation::file_ops

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_SRC_FILE_OPS_HPP
