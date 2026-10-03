# Contributing to Facility Capacity Reservation

Facility Capacity Reservation is part of Data Center Control Plane (DCCP),
Tranche 2 (Facility Capacity and Placement), and is maintained by Summon
Software Labs.

## Licensing of contributions

This project is licensed under the Apache License, Version 2.0 (see `LICENSE`).

By submitting a contribution you agree that it is licensed under the terms of
that license, as described in section 5 of the license text. There is **no
Contributor License Agreement** to sign, and no copyright assignment is
required: you keep the copyright in your contribution and grant the project the
license described in `LICENSE`.

Please do not add co-author trailers or attribution lines that name tools,
assistants or intermediate processes; commit authorship is the responsibility of
the human contributor.

## What belongs in this repository

Facility Capacity Reservation owns **facility-capacity commitments** across
space, rack, power, cooling and explicit facility services:

* reservation identity, per-reservation generations and writer authority epochs;
* typed multi-resource claims with exact units and atomic all-or-nothing binding;
* exact preconditions on the consumed capacity-snapshot source generation;
* headroom classes and the free / committed / protected accounting closure;
* validity intervals, lifecycle transitions and expiry;
* amendment lineage, idempotent attempt replay and durable attempt identity;
* release, expiry and revocation provenance;
* revalidation and reconciliation against newly consumed capacity snapshots;
* versioned, integrity-checked, crash-safe persistence and process authority.

It deliberately does **not** own:

* the derivation of facility capacity, or the capacity snapshots it consumes
  (Facility Capacity and the source capacity runtimes);
* placement selection or placement optimisation (Facility Placement Planner);
* rack membership, occupancy or naming (Rack Registry);
* asset inventory (Asset Registry);
* electrical or thermal actuation (Power Control, Cooling Control);
* accelerator compute, memory, serving or workload scheduling
  (Accelerated Systems Infrastructure);
* network topology, paths, transport or bandwidth reservation
  (Distributed Fabric Infrastructure);
* structural facility state (Facility Topology) or facility-state provenance
  (Facility State Ledger).

External ASI, DFI and DCCP objects may be referenced only through stable opaque
identifiers or typed references. Do not reimplement their semantics here, and do
not add hardware integration that cannot be exercised.

## Building and testing

The project is a portable C++20 CMake project. From a clean checkout:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Requirements:

* CMake 3.20 or newer;
* a C++20 compiler (MSVC 19.30+, GCC 11+, or Clang 14+);
* no third-party dependencies.

All first-party code must build with zero warnings. The default configuration
treats warnings as errors (`FACILITY_CAPACITY_RESERVATION_WARNINGS_AS_ERRORS=ON`);
fix the cause rather than suppressing the warning.

## Expectations for a change

* **Correctness before coverage.** A change to authoritative state should come
  with proof: a test that fails before the change and passes after it.
* **Test the boundary, not just the happy path.** Persisted state, canonical
  documents, imported capacity snapshots and CLI arguments are untrusted input.
  Malformed, truncated, oversized, reordered, duplicated and corrupted inputs
  must be rejected with a stable error code.
* **Atomicity is not negotiable.** A multi-resource commitment is applied in
  full or not at all, and never becomes visible before it is durable.
* **Authority is explicit.** Every mutation that depends on current state must
  carry its preconditions: reservation generation, writer authority epoch,
  expected revision and expected source generation. A stale precondition is
  refused, never merged.
* **Accounting must close.** `committed + protected + free == reservable` for
  every capacity pool at every published revision. The closure is checked before
  publication and re-derived independently by `verify()`.
* **Determinism.** Where the library claims deterministic behaviour — canonical
  ordering, serialization, reconciliation order, rejection — the behaviour must
  be encoded in a test, not left to incidental container behaviour.
* **Stable vocabulary.** Error codes and canonical tokens are a public contract.
  Extend them; never renumber or repurpose an existing value.
* **Bounded resources.** Validate declared sizes, counts and limits before
  allocating. Bound document sizes, claim counts, lineage lengths and the
  attempt index.
* **Exact arithmetic.** Capacity, counters, intervals and generations are
  checked integers. No floating-point authoritative accounting; overflow is
  reported, never wrapped.
* **Durability discipline.** Authoritative state changes only through the
  documented protocol: plan, validate, reserve generation and attempt, write
  staging, flush, verify, atomically publish the state generation, atomically
  commit the head pointer, then retire superseded state.

## Style

* C++20, standard library only.
* Formatting follows the surrounding code: two-space indentation, 120-column
  target, `snake_case` for functions and variables, `PascalCase` for types,
  trailing `_` for private data members.
* Public headers live under `include/dccp/facility_capacity_reservation/` and
  must be self-contained.
* Every file starts with the SPDX identifier and the copyright line:

  ```
  // SPDX-License-Identifier: Apache-2.0
  // Copyright 2026 Summon Software Labs.
  ```

## Reporting a problem

Open an issue with a minimal reproduction: the exact command, the exact input
(for a capacity snapshot or a persisted state file, the smallest document that
shows the problem), the observed result and the expected result. If a property
test failed, include the seed — every randomized test prints the seed it ran
with.
