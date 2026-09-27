// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "file_ops.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dccp::facility_capacity_reservation::file_ops {
namespace {

#if defined(_WIN32)
std::string describe_system_error(int code) { return std::system_category().message(code); }
#endif

std::error_code last_system_error() { return std::error_code(errno, std::system_category()); }

/// Refuses to replace anything that is not a regular file or absent. A symbolic
/// link, junction, device or directory in the place of a state file is untrusted
/// input, not something to write through.
Result<void> require_regular_or_missing(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
  if (error) {
    if (error == std::errc::no_such_file_or_directory) {
      return ok();
    }
    return Error(ErrorCode::IoError, "cannot inspect path: " + error.message()).with_subject(path.string());
  }
  if (status.type() == std::filesystem::file_type::not_found) {
    return ok();
  }
  if (status.type() != std::filesystem::file_type::regular) {
    return Error(ErrorCode::SymlinkRefused,
                 "refusing to replace a path that is not a regular file (symbolic links, junctions and "
                 "special files are rejected)")
        .with_subject(path.string());
  }
  return ok();
}

bool is_control_byte(char value) noexcept {
  const unsigned char byte = static_cast<unsigned char>(value);
  return byte < 0x20 || byte == 0x7F;
}

}  // namespace

std::string process_id_token() {
#if defined(_WIN32)
  return std::to_string(static_cast<unsigned long>(::GetCurrentProcessId()));
#else
  return std::to_string(static_cast<long>(::getpid()));
#endif
}

std::string next_sequence_token() {
  static std::atomic<std::uint64_t> counter{0};
  return std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

bool is_safe_file_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > 255) {
    return false;
  }
  if (name == "." || name == "..") {
    return false;
  }
  for (const char character : name) {
    if (character == '/' || character == '\\' || character == ':' || character == '\0') {
      return false;
    }
    if (is_control_byte(character)) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// FileLock
// ---------------------------------------------------------------------------

struct FileLock::Handle {
#if defined(_WIN32)
  HANDLE handle = INVALID_HANDLE_VALUE;
#else
  int descriptor = -1;
#endif
};

FileLock::FileLock() noexcept = default;
FileLock::FileLock(std::unique_ptr<Handle> handle) noexcept : handle_(std::move(handle)) {}

FileLock::~FileLock() { release(); }

FileLock::FileLock(FileLock&& other) noexcept : handle_(std::move(other.handle_)) {}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = std::move(other.handle_);
  }
  return *this;
}

bool FileLock::held() const noexcept { return handle_ != nullptr; }

void FileLock::release() noexcept {
  if (handle_ == nullptr) {
    return;
  }
#if defined(_WIN32)
  if (handle_->handle != INVALID_HANDLE_VALUE) {
    OVERLAPPED overlapped{};
    ::UnlockFileEx(handle_->handle, 0, 1, 0, &overlapped);
    ::CloseHandle(handle_->handle);
    handle_->handle = INVALID_HANDLE_VALUE;
  }
#else
  if (handle_->descriptor >= 0) {
    ::flock(handle_->descriptor, LOCK_UN);
    ::close(handle_->descriptor);
    handle_->descriptor = -1;
  }
#endif
  handle_.reset();
}

Result<FileLock> FileLock::acquire(const std::filesystem::path& file, LockMode mode) {
  // A lock file that is a symbolic link would let two processes lock different
  // files while each believed it held the store, which is exactly the mutual
  // exclusion this lock exists to provide. The link is refused, not followed.
  {
    std::error_code status_error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(file, status_error);
    if (!status_error && status.type() == std::filesystem::file_type::symlink) {
      return Error(ErrorCode::SymlinkRefused, "refusing to take the store lock through a symbolic link")
          .with_subject(file.string());
    }
    if (!status_error && status.type() != std::filesystem::file_type::not_found &&
        status.type() != std::filesystem::file_type::regular) {
      return Error(ErrorCode::SymlinkRefused,
                   "refusing to take the store lock through a path that is not a regular file")
          .with_subject(file.string());
    }
  }

  auto handle = std::make_unique<Handle>();
#if defined(_WIN32)
  const std::wstring wide = file.wstring();
  handle->handle = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle->handle == INVALID_HANDLE_VALUE) {
    return Error(ErrorCode::LockUnavailable,
                 "cannot open the store lock file: " + describe_system_error(static_cast<int>(::GetLastError())))
        .with_subject(file.string());
  }
  OVERLAPPED overlapped{};
  DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
  if (mode == LockMode::Exclusive) {
    flags |= LOCKFILE_EXCLUSIVE_LOCK;
  }
  if (::LockFileEx(handle->handle, flags, 0, 1, 0, &overlapped) == 0) {
    const DWORD code = ::GetLastError();
    ::CloseHandle(handle->handle);
    if (code == ERROR_LOCK_VIOLATION || code == ERROR_IO_PENDING) {
      return Error(ErrorCode::StoreLocked, "another process holds the store lock").with_subject(file.string());
    }
    return Error(ErrorCode::LockUnavailable,
                 "cannot lock the store lock file: " + describe_system_error(static_cast<int>(code)))
        .with_subject(file.string());
  }
#else
  handle->descriptor = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
  if (handle->descriptor < 0) {
    return Error(ErrorCode::LockUnavailable,
                 "cannot open the store lock file: " + describe_system_error(last_system_error().value()))
        .with_subject(file.string());
  }
  const int operation = (mode == LockMode::Exclusive) ? (LOCK_EX | LOCK_NB) : (LOCK_SH | LOCK_NB);
  if (::flock(handle->descriptor, operation) != 0) {
    const std::error_code error = last_system_error();
    ::close(handle->descriptor);
    handle->descriptor = -1;
    if (error.value() == EWOULDBLOCK || error.value() == EAGAIN) {
      return Error(ErrorCode::StoreLocked, "another process holds the store lock").with_subject(file.string());
    }
    return Error(ErrorCode::LockUnavailable, "cannot lock the store lock file: " + error.message())
        .with_subject(file.string());
  }
#endif
  return FileLock(std::move(handle));
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

Result<std::optional<std::string>> read_file_if_exists(const std::filesystem::path& path, std::size_t max_bytes) {
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
  if (error) {
    if (error == std::errc::no_such_file_or_directory) {
      return std::optional<std::string>();
    }
    return Error(ErrorCode::IoError, "cannot inspect file: " + error.message()).with_subject(path.string());
  }
  if (status.type() == std::filesystem::file_type::not_found) {
    return std::optional<std::string>();
  }
  if (status.type() == std::filesystem::file_type::symlink) {
    return Error(ErrorCode::SymlinkRefused, "refusing to read through a symbolic link").with_subject(path.string());
  }
  if (status.type() != std::filesystem::file_type::regular) {
    return Error(ErrorCode::PathInvalid, "refusing to read a path that is not a regular file")
        .with_subject(path.string());
  }
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    return Error(ErrorCode::IoError, "cannot determine file size: " + error.message()).with_subject(path.string());
  }
  if (size > max_bytes) {
    return Error(ErrorCode::LimitExceeded, "file exceeds the configured maximum size").with_subject(path.string());
  }

  std::string content;
  content.reserve(static_cast<std::size_t>(size));
  std::FILE* stream = nullptr;
#if defined(_WIN32)
  if (::_wfopen_s(&stream, path.wstring().c_str(), L"rb") != 0) {
    stream = nullptr;
  }
#else
  stream = std::fopen(path.c_str(), "rb");
#endif
  if (stream == nullptr) {
    return Error(ErrorCode::IoError, "cannot open file for reading").with_subject(path.string());
  }
  char buffer[64 * 1024];
  std::size_t total = 0;
  for (;;) {
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer), stream);
    if (read > 0) {
      total += read;
      if (total > max_bytes) {
        std::fclose(stream);
        return Error(ErrorCode::LimitExceeded, "file grew past the configured maximum size while being read")
            .with_subject(path.string());
      }
      content.append(buffer, read);
    }
    if (read < sizeof(buffer)) {
      if (std::ferror(stream) != 0) {
        std::fclose(stream);
        return Error(ErrorCode::IoError, "read failure").with_subject(path.string());
      }
      break;
    }
  }
  std::fclose(stream);
  return std::optional<std::string>(std::move(content));
}

Result<std::string> read_file(const std::filesystem::path& path, std::size_t max_bytes) {
  FCR_TRY(content, read_file_if_exists(path, max_bytes));
  if (!content.has_value()) {
    return Error(ErrorCode::StoreNotFound, "file does not exist").with_subject(path.string());
  }
  return std::move(*content);
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

Result<void> atomic_write_file(const std::filesystem::path& path, std::string_view content) {
  FCR_TRYV(require_regular_or_missing(path));
  std::filesystem::path directory = path.parent_path();
  if (directory.empty()) {
    directory = std::filesystem::path(".");
  }
  const std::filesystem::path temporary =
      directory / (path.filename().string() + ".tmp-" + process_id_token() + "-" + next_sequence_token());

#if defined(_WIN32)
  const std::wstring wide = temporary.wstring();
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error(ErrorCode::IoError,
                 "cannot create temporary file: " + describe_system_error(static_cast<int>(::GetLastError())))
        .with_subject(temporary.string());
  }
  std::size_t written = 0;
  while (written < content.size()) {
    const std::size_t chunk = std::min<std::size_t>(content.size() - written, 1U << 20);
    DWORD chunk_written = 0;
    if (::WriteFile(handle, content.data() + written, static_cast<DWORD>(chunk), &chunk_written, nullptr) == 0) {
      const DWORD code = ::GetLastError();
      ::CloseHandle(handle);
      ::DeleteFileW(wide.c_str());
      return Error(ErrorCode::IoError,
                   "cannot write temporary file: " + describe_system_error(static_cast<int>(code)))
          .with_subject(temporary.string());
    }
    written += chunk_written;
  }
  if (::FlushFileBuffers(handle) == 0) {
    const DWORD code = ::GetLastError();
    ::CloseHandle(handle);
    ::DeleteFileW(wide.c_str());
    return Error(ErrorCode::IoError,
                 "cannot flush temporary file: " + describe_system_error(static_cast<int>(code)))
        .with_subject(temporary.string());
  }
  if (::CloseHandle(handle) == 0) {
    ::DeleteFileW(wide.c_str());
    return Error(ErrorCode::IoError, "cannot close temporary file").with_subject(temporary.string());
  }
#else
  const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (descriptor < 0) {
    return Error(ErrorCode::IoError,
                 "cannot create temporary file: " + describe_system_error(last_system_error().value()))
        .with_subject(temporary.string());
  }
  std::size_t written = 0;
  while (written < content.size()) {
    const ssize_t chunk = ::write(descriptor, content.data() + written, content.size() - written);
    if (chunk <= 0) {
      const std::error_code error = last_system_error();
      ::close(descriptor);
      ::unlink(temporary.c_str());
      return Error(ErrorCode::IoError, "cannot write temporary file: " + error.message())
          .with_subject(temporary.string());
    }
    written += static_cast<std::size_t>(chunk);
  }
  if (::fsync(descriptor) != 0) {
    const std::error_code error = last_system_error();
    ::close(descriptor);
    ::unlink(temporary.c_str());
    return Error(ErrorCode::IoError, "cannot flush temporary file: " + error.message()).with_subject(temporary.string());
  }
  if (::close(descriptor) != 0) {
    ::unlink(temporary.c_str());
    return Error(ErrorCode::IoError, "cannot close temporary file").with_subject(temporary.string());
  }
#endif

  // Verify the staged bytes before they replace anything: a short write or a
  // failing device is caught here, while the previous content is still intact.
  FCR_TRY(staged, read_file(temporary, content.size() + 1));
  if (staged != content) {
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    return Error(ErrorCode::IntegrityFailure, "the staged file does not match the bytes that were written")
        .with_subject(path.string());
  }

#if defined(_WIN32)
  if (::MoveFileExW(wide.c_str(), path.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    const DWORD code = ::GetLastError();
    ::DeleteFileW(wide.c_str());
    return Error(ErrorCode::IoError,
                 "cannot publish temporary file: " + describe_system_error(static_cast<int>(code)))
        .with_subject(path.string());
  }
#else
  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    const std::error_code error = last_system_error();
    ::unlink(temporary.c_str());
    return Error(ErrorCode::IoError, "cannot publish temporary file: " + error.message()).with_subject(path.string());
  }
#endif
  return flush_directory(directory);
}

Result<void> flush_directory(const std::filesystem::path& directory) {
#if defined(_WIN32)
  (void)directory;
  // Windows does not expose a portable directory flush. The staged file is
  // flushed with FlushFileBuffers and the replace is issued with
  // MOVEFILE_WRITE_THROUGH, which is the strongest guarantee the platform
  // offers; this is recorded as a platform limitation rather than claimed as a
  // directory-durability proof.
  return ok();
#else
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return ok();
  }
  const int result = ::fsync(descriptor);
  const std::error_code error = last_system_error();
  ::close(descriptor);
  if (result != 0 && error.value() != EINVAL) {
    return Error(ErrorCode::IoError, "cannot flush directory: " + error.message()).with_subject(directory.string());
  }
  return ok();
#endif
}

Result<void> create_directories(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return Error(ErrorCode::IoError, "cannot create directory: " + error.message()).with_subject(directory.string());
  }
  if (!std::filesystem::is_directory(directory, error) || error) {
    return Error(ErrorCode::PathInvalid, "path exists but is not a directory").with_subject(directory.string());
  }
  return ok();
}

bool path_is_regular_file(const std::filesystem::path& path) noexcept {
  std::error_code error;
  return !error && std::filesystem::symlink_status(path, error).type() == std::filesystem::file_type::regular;
}

bool path_is_directory(const std::filesystem::path& path) noexcept {
  std::error_code error;
  const auto type = std::filesystem::symlink_status(path, error).type();
  return !error && type == std::filesystem::file_type::directory;
}

bool path_exists(const std::filesystem::path& path) noexcept {
  std::error_code error;
  const auto type = std::filesystem::symlink_status(path, error).type();
  return !error && type != std::filesystem::file_type::not_found;
}

Result<bool> directory_is_empty(const std::filesystem::path& directory) {
  if (!path_is_directory(directory)) {
    return Error(ErrorCode::PathInvalid, "path is not a directory").with_subject(directory.string());
  }
  std::error_code error;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) {
    return Error(ErrorCode::IoError, "cannot enumerate directory: " + error.message()).with_subject(directory.string());
  }
  return iterator == std::filesystem::directory_iterator();
}

Result<bool> remove_file(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) {
    return false;
  }
  if (error) {
    return Error(ErrorCode::IoError, "cannot inspect path: " + error.message()).with_subject(path.string());
  }
  if (status.type() == std::filesystem::file_type::not_found) {
    return false;
  }
  if (status.type() == std::filesystem::file_type::symlink) {
    // A link is removed, never followed, so cleanup cannot be turned into a
    // delete of an unrelated file.
    std::error_code link_error;
    const bool removed = std::filesystem::remove(path, link_error);
    if (link_error) {
      return Error(ErrorCode::IoError, "cannot remove symbolic link: " + link_error.message())
          .with_subject(path.string());
    }
    return removed;
  }
  const bool removed = std::filesystem::remove(path, error);
  if (error) {
    return Error(ErrorCode::IoError, "cannot remove file: " + error.message()).with_subject(path.string());
  }
  return removed;
}

Result<void> remove_tree(const std::filesystem::path& directory) {
  if (!path_exists(directory)) {
    return ok();
  }
  if (!path_is_directory(directory)) {
    return Error(ErrorCode::PathInvalid, "refusing to remove a path that is not a directory")
        .with_subject(directory.string());
  }
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  if (error) {
    return Error(ErrorCode::IoError, "cannot remove directory tree: " + error.message())
        .with_subject(directory.string());
  }
  return ok();
}

Result<void> rename_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
#if defined(_WIN32)
  if (::MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
      0) {
    return Error(ErrorCode::IoError,
                 "cannot rename: " + describe_system_error(static_cast<int>(::GetLastError())))
        .with_subject(from.string());
  }
#else
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return Error(ErrorCode::IoError, "cannot rename: " + describe_system_error(last_system_error().value()))
        .with_subject(from.string());
  }
#endif
  return ok();
}

Result<std::vector<std::string>> list_directory(const std::filesystem::path& directory) {
  // Bounded so that a directory filled by something other than this library
  // cannot drive an unbounded allocation.
  constexpr std::size_t kMaxDirectoryEntries = 1'000'000;

  std::vector<std::string> names;
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error) || error) {
    return names;
  }
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) {
    return Error(ErrorCode::IoError, "cannot enumerate directory: " + error.message()).with_subject(directory.string());
  }
  for (const std::filesystem::directory_entry& entry : iterator) {
    if (names.size() >= kMaxDirectoryEntries) {
      return Error(ErrorCode::LimitExceeded, "directory holds more entries than the configured maximum")
          .with_subject(directory.string());
    }
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace dccp::facility_capacity_reservation::file_ops
