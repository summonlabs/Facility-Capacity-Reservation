// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal helpers of the library, reachable only by the library, the test
// suite (which is compiled against the private include directory) and the
// inspection tool. Nothing here is part of the public contract.

#ifndef DCCP_FACILITY_CAPACITY_RESERVATION_DETAIL_STATUS_INTERNAL_HPP
#define DCCP_FACILITY_CAPACITY_RESERVATION_DETAIL_STATUS_INTERNAL_HPP

#include <cstddef>

namespace dccp::facility_capacity_reservation::detail {

/// Number of rows in the stable error-name table.
std::size_t error_code_count() noexcept;

/// True when the error-name table covers every enumerator exactly once, in
/// enumerator order, with distinct non-empty names.
bool error_vocabulary_complete() noexcept;

}  // namespace dccp::facility_capacity_reservation::detail

#endif  // DCCP_FACILITY_CAPACITY_RESERVATION_DETAIL_STATUS_INTERNAL_HPP
