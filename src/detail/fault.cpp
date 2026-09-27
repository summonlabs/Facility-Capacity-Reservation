// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "detail/fault.hpp"

#include <cstdlib>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dccp::facility_capacity_reservation::detail {
namespace {

/// Reads an environment variable.
///
/// The platform split exists because the C runtime's std::getenv is marked
/// deprecated by MSVC; the warning is not suppressed, the call is simply not
/// used there.
std::string read_environment(std::string_view name) {
#if defined(_WIN32)
  const std::string key(name);
  const DWORD needed = ::GetEnvironmentVariableA(key.c_str(), nullptr, 0);
  if (needed == 0) {
    return std::string();
  }
  std::string buffer(static_cast<std::size_t>(needed), '\0');
  const DWORD written = ::GetEnvironmentVariableA(key.c_str(), buffer.data(), needed);
  if (written == 0 || written >= needed) {
    return std::string();
  }
  buffer.resize(static_cast<std::size_t>(written));
  return buffer;
#else
  const std::string key(name);
  const char* value = std::getenv(key.c_str());
  return value == nullptr ? std::string() : std::string(value);
#endif
}

}  // namespace

PublishStage parse_publish_stage(std::string_view token) noexcept {
  if (token == "after-state-write") {
    return PublishStage::AfterStateWrite;
  }
  if (token == "before-head-commit") {
    return PublishStage::BeforeHeadCommit;
  }
  if (token == "after-head-commit") {
    return PublishStage::AfterHeadCommit;
  }
  return PublishStage::None;
}

PublishStage configured_publish_fault() noexcept {
  static const PublishStage stage = [] {
    const std::string value = read_environment(kFaultEnvironmentVariable);
    return parse_publish_stage(value);
  }();
  return stage;
}

void reach_publish_stage(PublishStage stage) noexcept {
  if (stage == PublishStage::None) {
    return;
  }
  if (configured_publish_fault() == stage) {
    // Terminate without unwinding: destructors do not run, buffers are not
    // flushed and no lock is released by this process's own code. That is the
    // point — it is the closest a portable test can get to a power loss.
    std::_Exit(kFaultExitStatus);
  }
}

}  // namespace dccp::facility_capacity_reservation::detail
