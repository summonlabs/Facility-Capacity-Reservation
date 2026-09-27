// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "child_process.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fcr_test {
namespace {

/// Process identifier as a decimal string, used only to keep capture files
/// distinct between simultaneously running parents.
std::string process_token() {
#if defined(_WIN32)
  return std::to_string(static_cast<unsigned long>(::GetCurrentProcessId()));
#else
  return std::to_string(static_cast<long>(::getpid()));
#endif
}

std::filesystem::path output_path() {
  static std::atomic<std::uint64_t> counter{0};
  std::error_code error;
  std::filesystem::path directory = std::filesystem::temp_directory_path(error);
  if (error) {
    directory = std::filesystem::current_path();
  }
  return directory /
         ("fcr_child_output_" + process_token() + "_" + std::to_string(counter.fetch_add(1)) + ".txt");
}

std::string read_and_remove(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  stream.close();
  std::error_code error;
  std::filesystem::remove(path, error);
  // The capture file receives the child's standard output and standard error as
  // the platform writes them. On Windows that means CRLF line endings, because
  // the C runtime translates text-mode writes. Normalising here keeps every
  // assertion in the suite about the child's text rather than the platform's
  // newline convention.
  std::string text = buffer.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

#if defined(_WIN32)

std::wstring to_wide(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), length);
  return wide;
}

/// Quotes one argument for a CreateProcess command line.
std::string quote_argument(const std::string& value) {
  if (!value.empty() && value.find_first_of(" \t\n\v\"") == std::string::npos) {
    return value;
  }
  std::string out;
  out.push_back('"');
  for (std::size_t index = 0; index < value.size(); ++index) {
    std::size_t backslashes = 0;
    while (index < value.size() && value[index] == '\\') {
      ++backslashes;
      ++index;
    }
    if (index == value.size()) {
      out.append(backslashes * 2, '\\');
      break;
    }
    if (value[index] == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
      continue;
    }
    out.append(backslashes, '\\');
    out.push_back(value[index]);
  }
  out.push_back('"');
  return out;
}

HANDLE open_capture(const std::filesystem::path& path) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  return ::CreateFileW(path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

HANDLE open_null_input() {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  return ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

#else

void write_all(int descriptor, std::string_view text) {
  std::size_t written = 0;
  while (written < text.size()) {
    const ssize_t chunk = ::write(descriptor, text.data() + written, text.size() - written);
    if (chunk <= 0) {
      return;
    }
    written += static_cast<std::size_t>(chunk);
  }
}

#endif

}  // namespace

std::filesystem::path executable_path() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(4096);
  const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return {};
  }
  return std::filesystem::path(std::wstring(buffer.data(), length));
#else
  std::vector<char> buffer(4096);
  const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length <= 0) {
    return {};
  }
  return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
#endif
}

struct ChildHandle::Impl {
  std::filesystem::path capture;
  int exit_code = -1;
#if defined(_WIN32)
  HANDLE process = nullptr;
  HANDLE thread = nullptr;
  HANDLE output = INVALID_HANDLE_VALUE;
  bool started = false;
#else
  pid_t pid = -1;
#endif
};

ChildHandle::ChildHandle(Impl* impl) noexcept : impl_(impl) {}

ChildHandle::ChildHandle(ChildHandle&& other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }

ChildHandle& ChildHandle::operator=(ChildHandle&& other) noexcept {
  if (this != &other) {
    if (impl_ != nullptr) {
      delete impl_;
    }
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

ChildHandle::~ChildHandle() { delete impl_; }

bool ChildHandle::valid() const noexcept { return impl_ != nullptr; }

ChildOutcome ChildHandle::wait() {
  ChildOutcome outcome;
  if (impl_ == nullptr) {
    return outcome;
  }
#if defined(_WIN32)
  if (impl_->started) {
    ::WaitForSingleObject(impl_->process, INFINITE);
    DWORD exit_code = 1;
    ::GetExitCodeProcess(impl_->process, &exit_code);
    ::CloseHandle(impl_->thread);
    ::CloseHandle(impl_->process);
    ::CloseHandle(impl_->output);
    impl_->exit_code = static_cast<int>(exit_code);
  }
#else
  if (impl_->pid > 0) {
    int status = 0;
    if (::waitpid(impl_->pid, &status, 0) >= 0) {
      if (WIFEXITED(status)) {
        impl_->exit_code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        impl_->exit_code = 128 + WTERMSIG(status);
      }
    }
  }
#endif
  outcome.exit_code = impl_->exit_code;
  outcome.output = read_and_remove(impl_->capture);
  delete impl_;
  impl_ = nullptr;
  return outcome;
}

ChildHandle start_child(const std::filesystem::path& program, const std::vector<std::string>& arguments) {
  auto* impl = new ChildHandle::Impl();
  impl->capture = output_path();
  if (program.empty()) {
    delete impl;
    return ChildHandle();
  }

#if defined(_WIN32)
  std::string command = quote_argument(program.string());
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command.append(quote_argument(argument));
  }
  std::wstring wide_command = to_wide(command);
  if (wide_command.empty()) {
    delete impl;
    return ChildHandle();
  }
  const HANDLE output = open_capture(impl->capture);
  if (output == INVALID_HANDLE_VALUE) {
    delete impl;
    return ChildHandle();
  }
  const HANDLE input = open_null_input();

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output;
  startup.hStdError = output;
  startup.hStdInput = (input == INVALID_HANDLE_VALUE) ? nullptr : input;

  PROCESS_INFORMATION process{};
  const BOOL created = ::CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                        &startup, &process);
  if (input != INVALID_HANDLE_VALUE) {
    ::CloseHandle(input);
  }
  if (created == 0) {
    ::CloseHandle(output);
    delete impl;
    return ChildHandle();
  }
  impl->process = process.hProcess;
  impl->thread = process.hThread;
  impl->output = output;
  impl->started = true;
  return ChildHandle(impl);
#else
  std::vector<std::string> storage;
  storage.push_back(program.string());
  for (const std::string& argument : arguments) {
    storage.push_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& value : storage) {
    argv.push_back(value.data());
  }
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    delete impl;
    return ChildHandle();
  }
  if (pid == 0) {
    const int descriptor = ::open(impl->capture.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor >= 0) {
      ::dup2(descriptor, STDOUT_FILENO);
      ::dup2(descriptor, STDERR_FILENO);
      ::close(descriptor);
    }
    ::execv(program.c_str(), argv.data());
    write_all(STDERR_FILENO, "cannot execute the child process\n");
    ::_exit(127);
  }
  impl->pid = pid;
  return ChildHandle(impl);
#endif
}

ChildHandle start_child(const std::vector<std::string>& arguments) {
  return start_child(executable_path(), arguments);
}

ChildOutcome run_program(const std::filesystem::path& program, const std::vector<std::string>& arguments) {
  ChildHandle handle = start_child(program, arguments);
  if (!handle.valid()) {
    ChildOutcome outcome;
    outcome.output = "the child process could not be started";
    return outcome;
  }
  return handle.wait();
}

ChildOutcome run_child(const std::vector<std::string>& arguments) {
  return run_program(executable_path(), arguments);
}

bool dispatch_child_scenario(int argc, char** argv, int& exit_code) {
  if (argc < 2) {
    return false;
  }
  if (std::string(argv[1]) != "--fcr-child") {
    return false;
  }
  exit_code = child_scenarios_main(argc, argv);
  return true;
}

bool output_contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

std::string output_value(const std::string& text, const std::string& key) {
  const std::string prefix = key + "=";
  std::size_t position = 0;
  while (position < text.size()) {
    const std::size_t end = text.find('\n', position);
    const std::string line = text.substr(position, end == std::string::npos ? std::string::npos : end - position);
    if (line.rfind(prefix, 0) == 0) {
      return line.substr(prefix.size());
    }
    if (end == std::string::npos) {
      break;
    }
    position = end + 1;
  }
  return std::string();
}

}  // namespace fcr_test
