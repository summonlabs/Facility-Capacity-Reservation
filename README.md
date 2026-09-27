# Facility Capacity Reservation

Facility Capacity Reservation is repository 15 of the canonical 72-runtime Data
Center Control Plane (DCCP), in Tranche 2: Facility Capacity and Placement. It is
the authority that turns physical facility limits into explicit capacity
commitments: it decides whether a facility-capacity commitment can be made
against an exact capacity generation, which resources it binds and for how long,
and when it must expire, be released, revoked or fenced as stale.

The primary product is the C++20 library target `facility_capacity_reservation`,
exported as `dccp::facility_capacity_reservation`.

## 1. Systems boundary

### What this repository owns

* reservation identity, per-reservation generations, and writer authority epochs;
* typed multi-resource claims over space, rack, power, cooling and explicit
  facility services, bound atomically as one commitment;
* the exact capacity-snapshot source generation a commitment was priced against;
* headroom classes and the free / committed / protected accounting closure;
* validity intervals, lifecycle transitions, and expiry;
* amendment lineage, idempotent attempt replay and durable attempt identity;
* release, expiry and revocation provenance;
* revalidation and reconciliation against newly consumed capacity snapshots;
* versioned, integrity-checked, crash-safe persistence and process authority.

### What this repository explicitly does not own

| Adjacent authority | Boundary |
| --- | --- |
| Facility Capacity and the source capacity runtimes | Derive facility capacity. This library only *consumes* a capacity snapshot document and records the source generation it came from. It never computes capacity. |
| Accelerated Systems Infrastructure (ASI) | Accelerator execution, memory, serving and workload scheduling. No accelerator or accelerator-memory resource kind exists in this library's vocabulary. |
| Distributed Fabric Infrastructure (DFI) | Network topology, paths, transport and bandwidth. No bandwidth resource kind exists in this library's vocabulary. |
| Facility Placement Planner | Choosing *where* a commitment lands. A claim names a capacity pool scope supplied by the caller; this library never selects or optimises a placement. |
| Rack Registry, Asset Registry | Rack membership, occupancy and asset inventory. No such store is opened, read or written. |
| Power Control, Cooling Control | Electrical and thermal actuation. No device is contacted; power and cooling are integers in a claim. |
| Facility Topology, Facility State Ledger | Structural facility state and its provenance. Referenced only through opaque scope references. |

The separation is enforced by the type system rather than by convention: a
claimant reference is not an actor reference, a source generation is not a
reservation generation, and there is no resource kind for accelerator compute or
network bandwidth to claim in the first place.

### Scale and evidence

Everything here is control-plane and modelling infrastructure. This repository
was exercised on **synthetic** capacity figures, in-memory registers and real
files on one Windows machine. No PDU, UPS, generator, cooling unit, GPU, RDMA
link or multi-site fabric was involved at any point, and no such behaviour is
claimed.

## 2. Architecture

```
include/dccp/facility_capacity_reservation/
  status.hpp        stable outcome codes, Error, Result
  text.hpp          bounded canonical text primitives
  strong_id.hpp     identities, counters, generations, epochs, revisions
  units.hpp         exact integer capacity units and checked arithmetic
  digest.hpp        SHA-256 (FIPS 180-4)
  capacity.hpp      the consumed capacity snapshot
  reservation.hpp   claims, headroom, lifecycle, lineage, provenance
  request.hpp       operation requests and their deterministic outcomes
  ledger.hpp        the authoritative in-memory reservation ledger
  store.hpp         the durable store: locking, publication, recovery
  clock.hpp         logical tick source
  version.hpp       compiled-in version
src/
  status.cpp  text.cpp  strong_id.cpp  digest.cpp
  capacity.cpp  reservation.cpp
  ledger.cpp  ledger_ops.cpp         construction, accounting, staged operations
  codec.cpp                          canonical state serialization
  store.cpp                          publication, recovery, locking
  file_ops.cpp                       platform file primitives
  detail/                            internal helpers, codec, fault seam
tools/fcrctl          inspection and administration CLI
examples/             three runnable examples
benchmarks/           completed-operation benchmarks
tests/                the contract, property, adversarial and multiprocess suites
```

### The ledger is a value type

`ReservationLedger` owns no locks and no callbacks. Every mutating entry point is
a *staged* operation: it validates the request, applies the whole change to a
scratch copy, and lets the caller decide when to adopt it. A failed operation
leaves the authoritative ledger exactly as it was, which is what makes
multi-resource atomicity and crash-safe publication fall out of the same
structure: a durable caller holds the staged copy, publishes it, and only then
adopts it.

### The store holds exactly two locks

1. An advisory **inter-process file lock**, taken by `create()` / `open()` /
   `open_read_only()` and held for the whole lifetime of the store object. It is
   never acquired while any other lock is held. A process that dies holding it
   releases it through the operating system.
2. One process-local, non-recursive `std::mutex` that serialises mutations.

The lock order is therefore: file lock (already held, never re-acquired), then
mutex. Nothing acquires the file lock while holding the mutex, and nothing
acquires the mutex twice. The library invokes no caller-supplied callback
anywhere — there is no observer, no listener and no event sink in the public
surface — so a callback cannot re-enter a locked store.

The full audit is recorded in `store.hpp` and summarised in section 7.

## 3. State and authority model

### Identity is separate from metadata

`ReservationId` names a commitment for its whole life. Amending a reservation
produces a **new generation** of the same identity and never rewrites history:
the previous claim set, validity, priority and headroom class live on only in the
amendment lineage, which records generation, predecessor, attempt, revision,
authority epoch, tick, cause and a bounded detail.

### Observation is separate from authority

* A **read-only handle** takes a shared lock and verifies everything it reads; it
  refuses every mutation with `STORE_READ_ONLY`.
* `revalidate()` is a pure observation: it publishes nothing, consumes no attempt
  identity, and carries the exact revision it observed so a report can never be
  mistaken for a statement about a later state.
* `reconcile(mode = Observe)` reports the over-commit a new snapshot would cause
  and changes nothing at all.

### Capacity is separate from authority to consume it

A capacity snapshot is real capacity the moment it is consumed. It is no longer
*known to be current* the moment it is restored from persistence. The library
keeps the two apart:

* `install_capacity()` and a successful `reconcile()` mark the held evidence
  **current**;
* opening a store marks it **restored**;
* `reserve()` and `amend()` — the operations that add load — refuse restored
  evidence with `CAPACITY_EVIDENCE_STALE`;
* release, expiry, revocation, revalidation and reconciliation do not, because
  they cannot add load to the facility.

`StoreOptions::trust_persisted_capacity` is the explicit, documented way to state
that restored evidence is to be treated as current. It is off by default, and
`fcrctl` exposes it as `--trust-persisted-capacity`.

### What a commitment records

Every generation records the exact `(snapshot ref, source generation)` it was
priced against. A commitment whose source generation is older than the capacity
the ledger now holds is reported as **stale** — by `ReservationView::source_stale`
and by `revalidate()`. Reconciliation never re-prices a surviving commitment on
its claimant's behalf: the claimant must amend it.

## 4. Accounting

Capacity is consumed per pool, and a pool is the pair `(resource kind, scope
reference)`. The kinds are closed and carry fixed units:

| Kind | Canonical token | Unit |
| --- | --- | --- |
| `Space` | `space` | one space slot of the scope |
| `Rack` | `rack` | one rack position within the scope |
| `Power` | `power` | one watt of electrical capacity |
| `Cooling` | `cooling` | one watt of heat rejection capacity |
| `FacilityService` | `facility-service` | one unit of an explicitly named service |

The snapshot declares, per pool, `gross`, `withdrawn` and `floor`. The library
derives `available = gross - withdrawn` and `reservable = available - floor`,
both with checked subtraction; an inconsistent pool is refused rather than
clamped.

Headroom decides the bucket:

* `Guaranteed` and `Firm` are counted as **committed**;
* `Opportunistic` is counted as **protected** — a facility sells this capacity
  twice and can reclaim it.

The invariant is exact and holds at every published revision, for every pool:

```
committed + protected + free == reservable
```

It is checked with wrapping-free arithmetic before any state is published, and
`verify()` recomputes the whole accounting by a second, independent traversal and
compares it with the recorded split. The test suite additionally compares the
library against a reference model written from the definition rather than from
the implementation (`tests/test_support.cpp`). All authoritative capacity is
`std::uint64_t`; there is no floating-point capacity anywhere in the library.

## 5. Persistence, publication and recovery

### Store layout

```
<root>/
  fcr.lock            advisory inter-process lock; empty
  fcr.meta            store identity: sealed format version + incarnation
  fcr.head            the commit point: sealed head pointer
  generations/
    state-<revision>-<digest16>.fcr     the committed state generation
```

### Publication protocol

1. plan — build the candidate state in a scratch copy;
2. validate — closure and structural invariants;
3. reserve generation and attempt — advance the revision, bind the attempt;
4. write staging — encode and write `state-<revision>-<digest>.fcr` to a
   uniquely named temporary file in `generations/`;
5. flush — `FlushFileBuffers` (Windows) or `fsync` (POSIX);
6. verify — read the staged bytes back and compare them with what was written;
7. atomic publish — rename the temporary over the generation file;
8. verify again — read the file at its final name and re-derive its digest;
9. **commit point** — atomically replace `fcr.head` with a pointer to the new
   generation;
10. cleanup — retire every generation the head does not reference.

A crash at any step leaves either the previous committed state or the new one,
never a mixture. The in-memory state is adopted only after the head commit, so it
can never be ahead of the durable one.

### What recovery refuses

* A **missing head pointer** is not repaired. Rebuilding it from the newest
  generation on disk would promote a state whose commit was never acknowledged —
  a mutation whose caller was told it failed. `open()` reports `HEAD_MISSING` and
  the operator restores from backup.
* A head pointer that is corrupt, truncated, sealed wrongly, names a generation
  that escapes its directory, refers to a missing or altered generation, or
  belongs to a different incarnation is refused with a specific code.
* Orphan generations — files the head does not reference — are removed on open
  and after every successful publish. A file the store did not write is left
  alone: cleanup never removes something it cannot identify.
* A state that fails to decode, whose digest does not match, or whose accounting
  does not close is refused; nothing is adopted, and the store remains openable
  once the damage is repaired.

### Writer authority across restarts

Every `open()` advances the writer authority epoch and publishes the handover
*before* the store accepts any mutation. An operation carrying an older epoch is
refused with `STALE_AUTHORITY_EPOCH`. A process that dies holding the store
releases the lock through the operating system; the next process takes authority,
advances the epoch, and any decision taken under the dead writer's epoch is
fenced.

### Attempt identity is durable

The attempt index is persisted with the state, so a retry after a restart replays
the recorded outcome instead of acting twice. The index is a bounded, oldest-first
cache: an evicted attempt is no longer replayable, and a retry of one is reported
as a conflict rather than applied twice. This bound is explicit and configurable
(`LedgerOptions::max_attempts`).

## 6. Deterministic outcomes

All expected failures are reported through `Result<T>` and a stable
`ErrorCode`; the library never uses exceptions for them. 86 codes are defined in
the public vocabulary and are appended to, never renumbered or repurposed.
`error_code_name()` returns the stable upper-snake-case token that `fcrctl` prints
as `error-code=`.

`error_category()` groups them:

| Category | Examples |
| --- | --- |
| `ARGUMENT` | `INVALID_ARGUMENT`, `MALFORMED_IDENTIFIER`, `CLAIM_DUPLICATE_POOL`, `INVALID_INTERVAL`, `DIGEST_MISMATCH` |
| `CAPACITY` | `INSUFFICIENT_CAPACITY`, `POOL_UNKNOWN`, `CAPACITY_EVIDENCE_STALE`, `SOURCE_GENERATION_STALE`, `UNRESOLVABLE_OVERCOMMIT` |
| `RESERVATION` | `RESERVATION_NOT_FOUND`, `RESERVATION_ALREADY_EXISTS`, `RESERVATION_TERMINAL` |
| `AUTHORITY` | `STALE_AUTHORITY_EPOCH`, `STALE_REVISION`, `STALE_RESERVATION_GENERATION`, `ATTEMPT_CONFLICT`, `CLAIMANT_MISMATCH` |
| `PERSISTENCE` | `STORE_CORRUPT`, `HEAD_MISSING`, `HEAD_CORRUPT`, `INTEGRITY_FAILURE`, `SYMLINK_REFUSED`, `PATH_ESCAPES_ROOT` |
| `LIFECYCLE` | `STORE_LOCKED`, `STORE_CLOSED`, `STORE_READ_ONLY`, `OPERATION_NOT_PERMITTED` |
| `LIMIT` | `LIMIT_EXCEEDED`, `ARITHMETIC_OVERFLOW`, `LINEAGE_OVERFLOW` |
| `INTERNAL` | `INVARIANT_VIOLATION`, `ACCOUNTING_MISMATCH`, `INTERNAL_ERROR` |

### Validation precedence

Every staged operation applies the same ordered checks, so the code a caller
observes for a doubly-wrong request is predictable rather than incidental:

1. request shape — identifiers, limits, claim structure, interval;
2. attempt identity — new, replayable, or conflicting;
3. writer authority epoch;
4. expected state revision;
5. subject existence and bounds;
6. expected reservation generation;
7. reservation lifecycle state;
8. capability of the caller — claimant match, revocation policy;
9. claim-set change;
10. capacity evidence — installed, fresh, exact source generation;
11. capacity sufficiency — free units in every claimed pool.

Steps 1–9 are pure or read-only; 10–11 read evidence; nothing is written until
every check has passed. The order is asserted directly by
`ledger_basics.validation_precedence_is_fixed`.

## 7. Concurrency model

* Ownership and lock order are defined before implementation and recorded in
  `store.hpp`.
* Immutable snapshots: `Store::list()`, `find()`, `pools()` and every outcome
  return copies, so a caller never observes a state that is being replaced.
* Mutations are serialised by one non-recursive mutex; the cross-process lock is
  held for the store's lifetime and is never acquired under it.
* No callback, observer or event sink exists anywhere in the public surface, so
  there is no re-entrancy path through user code.
* `close()` takes the same mutex, so it cannot interleave with a mutation.

The nine audit points from the concurrency checklist were inspected by hand
against the final code: no read→write reacquisition on the same lock, no lock
held across a call that takes the same lock, no mutex re-entry through a
callback, no event emission beneath a lock, no join while holding a lock a worker
needs, no reversed nested ordering, no cancellation path with reversed ordering,
no progress callback re-entering mutable state, and no shutdown that waits on
work while preventing its completion.

The in-process half is exercised by `tests_concurrency.cpp` (threads), and the
cross-process half by `tests_multiprocess.cpp` (real operating-system processes).
Neither is claimed from the other.

## 8. API

```cpp
#include <dccp/facility_capacity_reservation/store.hpp>
namespace fcr = dccp::facility_capacity_reservation;

// Create or open a store.
fcr::Store store = fcr::Store::create("facility-store").value();
// or: fcr::Store::open(root), fcr::Store::open_read_only(root), fcr::Store::in_memory()

// Consume a capacity snapshot (parsed from a canonical document).
fcr::CapacitySnapshot snapshot =
    fcr::parse_capacity_snapshot(document, fcr::SnapshotLimits{}).value();
store.install_capacity(snapshot, fcr::Tick(1'000));

// Commit a multi-resource reservation against the exact generation consumed.
fcr::ReserveRequest request;
request.attempt   = fcr::AttemptId::parse("attempt-1", "attempt").value();
request.id        = fcr::ReservationId::parse("res-1", "id").value();
request.claimant  = fcr::ClaimantRef::parse("team-a", "claimant").value();
request.tenant    = fcr::TenantRef::parse("tenant-a", "tenant").value();
request.service   = fcr::ServiceRef::parse("svc-a", "service").value();
request.priority  = fcr::PriorityRef::parse("prio-normal", "priority").value();
request.headroom  = fcr::HeadroomClass::Firm;
request.authority.epoch = store.epoch().value();
request.expected_source_generation = snapshot.source_generation;
request.validity  = {fcr::Tick(1'100), fcr::Tick(900'000)};
request.now       = fcr::Tick(1'100);
request.claims    = { fcr::ResourceClaim{{fcr::ResourceKind::Rack,
                                          fcr::ScopeRef::parse("hall-a", "scope").value()}, 8},
                      fcr::ResourceClaim{{fcr::ResourceKind::Power,
                                          fcr::ScopeRef::parse("feed-a", "scope").value()}, 320'000} };

fcr::Result<fcr::ReserveOutcome> outcome = store.reserve(request);
if (!outcome) {
  // outcome.error().code() is a stable ErrorCode; .to_string() explains it.
}
```

The other operations follow the same shape: `amend`, `release`, `revoke`,
`expire`, `reconcile`, and the observations `revalidate`, `verify`, `status`,
`find`, `list`, `pools`, `accounting_of`.

Every mutating request carries the writer authority epoch, the expected
reservation generation where one is addressed, and the expected source
generation where capacity is consumed. A precondition that no longer holds is
refused; it is never merged into current state.

## 9. Inspection tool

`fcrctl` opens the store, issues exactly one request through the library, prints
a stable `key=value` record and closes. It holds no state between invocations and
writes nothing outside the store it was given.

```
fcrctl version
fcrctl init                 --root DIR
fcrctl snapshot-write       --out FILE --ref R --facility F --source-generation N \
                            --pool KIND:SCOPE:GROSS:WITHDRAWN:FLOOR...
fcrctl snapshot-show        --snapshot FILE
fcrctl install-capacity     --root DIR --snapshot FILE [--now TICK]
fcrctl status|list|pools|verify|revalidate   --root DIR
fcrctl show                 --root DIR --id ID
fcrctl reserve              --root DIR --id ID --claimant C --tenant T --service S --priority P \
                            --claim KIND:SCOPE:AMOUNT... --start TICK --deadline TICK --attempt A \
                            [--headroom guaranteed|firm|opportunistic]
fcrctl amend                --root DIR --id ID --generation N --actor A --attempt A \
                            --claim KIND:SCOPE:AMOUNT... --start TICK --deadline TICK [--cause TOKEN]
fcrctl release              --root DIR --id ID --generation N --actor A --attempt A
fcrctl revoke               --root DIR --id ID --generation N --actor A --attempt A --policy P \
                            [--allow-guaranteed-override]
fcrctl expire               --root DIR --attempt A [--now TICK]
fcrctl reconcile            --root DIR --snapshot FILE --expected-source-generation N --actor A \
                            --attempt A [--mode observe|enforce] [--policy P]
```

Common options: `--read-only`, `--quiet`, `--trust-persisted-capacity`.
Exit status is 0 for success, 1 for a reported error (with `error-code=`), and 2
for a usage error. Observation commands take a shared lock; mutating commands
take write authority; `--read-only` on a mutating command makes the library
refuse it with `STORE_READ_ONLY` rather than upgrading silently.

## 10. Build, test, install and consume

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /some/prefix
```

Requirements: CMake 3.20+, a C++20 compiler (MSVC 19.30+, GCC 11+, Clang 14+),
and no third-party dependencies. The build types are configured explicitly by
`CMakeLists.txt` so that a Release build is optimised even when the surrounding
toolchain supplies no per-configuration flags.

Options (all `ON` except ASan):

| Option | Effect |
| --- | --- |
| `FACILITY_CAPACITY_RESERVATION_BUILD_TESTS` | test suite and the downstream consumer test |
| `FACILITY_CAPACITY_RESERVATION_BUILD_TOOLS` | `fcrctl` (installed) |
| `FACILITY_CAPACITY_RESERVATION_BUILD_EXAMPLES` | the three examples |
| `FACILITY_CAPACITY_RESERVATION_BUILD_BENCHMARKS` | the benchmark executable |
| `FACILITY_CAPACITY_RESERVATION_WARNINGS_AS_ERRORS` | first-party warnings are errors |
| `FACILITY_CAPACITY_RESERVATION_ENABLE_ASAN` | AddressSanitizer (and UBSan on GCC/Clang) |

The installation carries the headers, the library, `fcrctl`, and a versioned
CMake package:

```cmake
find_package(facility_capacity_reservation 1.0 REQUIRED)
target_link_libraries(app PRIVATE dccp::facility_capacity_reservation)
```

`tests/downstream/` is an independent out-of-tree consumer that is built against
an installed prefix by `ctest` and run through a real lifecycle. It is not part
of this project's build graph.

### Examples

* `examples/basic_reservation.cpp` — consume a snapshot, commit a multi-resource
  reservation, amend it, release it, and check the closure at every step.
* `examples/durable_lifecycle.cpp` — create a store, commit, close, reopen, and
  show the epoch handover, the refusal of stale authority, and the refusal of
  restored capacity evidence.
* `examples/reconcile_capacity.cpp` — withdraw capacity, observe the over-commit,
  enforce the fencing order, and confirm the closure returns.

## 11. Validation performed

Everything below was executed on the primary platform (Windows 11, MSVC 19.44,
x64, Ninja, Release unless stated).

### Build quality

* **Release** and **Debug** configurations both build with **zero first-party
  warnings** under `/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /WX`. No warning
  is suppressed; nothing is disabled globally.
* Every configuration explicitly sets its optimisation flags, so a Release build
  is optimised even when the surrounding toolchain supplies no per-configuration
  flags.
* **AddressSanitizer**: the full test suite was run against a
  `RelWithDebInfo + /fsanitize=address` build — **301 tests, 0 failures, no
  sanitizer diagnostic**. The child processes the multiprocess suite spawns are
  the same instrumented binary, so they are covered as well. See section 13 for
  what this does and does not establish.

### Test suite

`facility_capacity_reservation_tests` — **301 tests in 22 suites**:

| Suite | Cases | What it establishes |
| --- | --- | --- |
| `status` | 10 | the error vocabulary is complete, ordered, distinct and categorised |
| `text` | 13 | canonical integers, the identifier alphabet, field encoding, strict line splitting |
| `digest` | 6 | SHA-256 against published FIPS 180-4 vectors, including the million-byte vector |
| `ids` | 9 | no identity family converts to another; counters report exhaustion instead of wrapping |
| `capacity` | 21 | snapshot parsing, canonical round-trip, every rejection |
| `ledger_basics` | 27 | installation, claim canonicality, acceptance, and the fixed validation precedence |
| `ledger_accounting` | 13 | the closure against the independent reference model through a full lifecycle |
| `ledger_atomicity` | 10 | all-or-nothing multi-resource binding; a rejection changes nothing observable |
| `ledger_lifecycle` | 15 | release, expiry, revocation, lineage, provenance, terminal-state rules |
| `ledger_idempotency` | 13 | replay, conflict, the bounded attempt index, durable attempt identity |
| `ledger_reconcile` | 16 | adoption, over-commit, deterministic fencing, the guaranteed-commitment boundary |
| `ledger_revalidate` | 12 | classification precedence, the revision precondition, restored-evidence rules |
| `codec` | 15 | exact round-tripping; a decoder stricter than the encoder |
| `store` | 19 | directory shape, durability-before-return, reopen, epoch handover, cleanup |
| `recovery` | 16 | what a real close/reopen preserves and what the store refuses to guess at |
| `corruption` | 15 | truncated, altered, oversized, swapped, symlinked and foreign state |
| `property` | 7 | seeded randomized workloads with the closure checked after every step |
| `adversarial` | 23 | deliberate attempts to make the library accept what it must refuse |
| `concurrency` | 5 | threads mutating and observing one store |
| `multiprocess` | 8 | real operating-system processes: exclusion, death, crash, race, reader |
| `internal` | 13 | the sealed documents, the fault seam, checked arithmetic, the version contract |
| `cli` | 15 | the tool driven through its real command line |

All 301 pass in Release, in Debug, and under AddressSanitizer.

### Property and randomized results

* Seven seeded randomized properties, six workloads each, with the closure
  checked against the reference model after **every** mutation: all passed. Each
  run prints the seed it used and every failure would print it too, so a failing
  case is reproducible with `--seed=<value>`.
* A rejection never changes the published revision: 200 randomized requests with
  the revision compared before and after each one.
* Accounting is never above the reservable capacity: 300 randomized reservations
  with the closure re-checked after each.
* Expiry is monotone in the sweep tick: every created commitment is ended exactly
  once across a sequence of increasing sweeps.
* State documents are byte-identical for identical states across four independent
  stores.

### Multiprocess and crash-recovery results

Eight scenarios, each a real second process re-executing the test binary:

* **Exclusion** — a second process is refused with `STORE_LOCKED` while the first
  holds the store, and succeeds once it is released.
* **Process death** — a child publishes a mutation and terminates without
  closing. The parent then takes authority: the epoch advanced (2 → 3), the
  child's mutation is intact, and `verify()` passes.
* **Crash during publication** — a child commits a mutation and dies at each of
  three stages of the protocol (`after-state-write`, `before-head-commit`,
  `after-head-commit`) with exit status 70. After each crash the store reopens
  into one whole state, the previously *acknowledged* mutation is present in
  every case, the crashed mutation is present exactly when the head commit had
  completed, the accounting closes, and exactly one generation file remains.
* **Concurrent writers** — six processes race on one store, each retrying up to
  25 times. Every mutation a child reported as accepted is present exactly once,
  the total matches the accounting, and `verify()` passes.
* **Reader** — a reader process is refused while a writer holds the store, then
  reads ten reservations, a closed accounting and non-fresh capacity evidence.
* **Stale epoch** — two processes die holding the store in turn; a third using
  the first one's epoch is refused with `STALE_AUTHORITY_EPOCH`.
* **Exclusive authority** — four children attempt 30 opens each; the epoch
  advances by exactly the number of successful opens, which is the number of
  handovers.
* **Parent and child writing at once** — the two contend on the same store and
  the final reservation count is exactly the sum of what each reported as
  accepted.

The fault seam used for the crash scenarios (`FCR_FAULT_INJECT`) is a documented
test facility: it terminates the publishing process at a named stage, and is
inert unless the variable is set. It is not product behaviour.

### Clang/GCC and sanitizer status

See section 13: only MSVC was exercised on this machine.

## 12. Benchmarks

`facility_capacity_reservation_benchmarks` measures **completed operations**,
including the durability work each operation actually performs: a durable reserve
figure contains the state encode, the staged write, the flush, the read-back
verification, the atomic publish and the head commit, because that is what the
call does.

Methodology:

* one process, fixed order, no other work in flight;
* a warm-up that is not measured, then the measured repetitions;
* both the **mean** and the **median** are reported, so a single slow flush
  cannot be mistaken for the typical cost;
* every workload states its size, and every benchmark verifies the accounting
  closure afterwards and removes the directory it created;
* **no before/after pair is published**: a controlled alternating run would be
  needed to make one meaningful, and none was performed;
* all workloads are **SYNTHETIC** — invented capacity, no facility hardware.

Measured on the primary platform (Windows 11, MSVC 19.44 x64, Release, `%TEMP%`
on the machine's system drive). The durable figures are dominated by the
platform's flush and metadata-flush latency and are therefore machine-specific;
the volatile figures measure the ledger itself.

| Benchmark | Mean | Median | Samples | Workload |
| --- | --- | --- | --- | --- |
| `reserve/volatile` | 109 µs | 110 µs | 500 | one completed commit into an in-memory ledger holding 100–600 reservations |
| `reserve+release/volatile` | 629 µs | 651 µs | 1000 | one completed commit and one completed release |
| `amend/volatile` | 92 µs | 86 µs | 1000 | one completed amendment of a reservation whose lineage reaches 1050 entries |
| `reserve/durable` | 14.1 ms | 13.7 ms | 500 | one completed durable commit: encode, write, flush, read-back verify, publish, head commit |
| `open+verify/durable` | 12.1 ms | 12.1 ms | 50 | reopen a 300-reservation store: lock, digest verification, decode, epoch handover, full verify |
| `expire-batch/durable` | 447 ms | 454 ms | 40 | 25 durable commits plus one durable sweep, all completed |
| `verify/300-reservations` | 2.35 ms | 2.29 ms | 60 | verify a 300-reservation store, including re-reading and re-decoding the committed artifact |

The durable figures are what a synchronous, fsync-per-step commit costs on this
machine: two staged writes, two flushes and two metadata-flushing renames per
mutation. They are dominated by the platform's flush latency and will differ on
other storage.

### Known performance characteristics

* A mutation stages a copy of the ledger, so its cost grows linearly with the
  number of reservations held. The measured volatile figures above show the
  slope; a facility holding a few thousand reservations pays a few hundred
  microseconds per mutation for it. This is a deliberate trade: it is what makes
  a failed operation provably leave the state untouched.
* `verify()` re-reads and re-decodes the committed artifact, so it costs the size
  of the store rather than a constant.

## 13. Genuine limitations

* **One platform was exercised.** Everything reported here ran on Windows 11 with
  MSVC 19.44 x64. The POSIX code paths in `src/file_ops.cpp` and
  `tests/child_process.cpp` (`flock`, `fsync`, `fork`/`execv`, directory fsync)
  are written and reason correctly, but they were **not compiled or run** on this
  machine. No POSIX result is claimed.
* **AddressSanitizer proves memory safety, not concurrency safety.** The suite
  was run against a `RelWithDebInfo + /fsanitize=address` build of the whole
  project, including the CLI exercised by the `cli` suite and the child processes
  the `multiprocess` suite spawns: 301 tests, 0 failures, no diagnostic. It does
  **not** prove the absence of data races — that needs ThreadSanitizer, which the
  MSVC toolchain does not provide, and ThreadSanitizer was therefore not run. The
  concurrency argument in section 7 rests on the lock discipline and on the
  thread and process suites, not on a race detector.
* **UndefinedBehaviourSanitizer was not run.** `/fsanitize=undefined` is a
  GCC/Clang facility; MSVC offers only the address sanitizer. The
  `FACILITY_CAPACITY_RESERVATION_ENABLE_ASAN` option enables
  `-fsanitize=address,undefined` for GCC and Clang, but no GCC or Clang build was
  executed on this machine, so no UBSan result is claimed.
* **Directory durability on Windows is as strong as the platform allows and no
  stronger.** `flush_directory()` is a no-op there because Windows exposes no
  portable directory flush; the staged file is flushed with `FlushFileBuffers`
  and the replace is issued with `MOVEFILE_WRITE_THROUGH`. On POSIX the directory
  is flushed with `fsync`. This is a documented platform difference, not a
  claimed directory-durability proof on Windows.
* **Crash recovery is proved by process termination, not by power loss.** The
  crash scenarios terminate a real process abruptly at named protocol stages.
  They do not and cannot prove behaviour under a power failure or a device that
  reorders its own writes.
* **Integrity is a digest, not a signature.** SHA-256 detects accidental
  corruption and truncation. It does not authenticate the writer; an attacker who
  can rewrite the store can rewrite the digest with it.
* **No hardware is involved.** No PDU, UPS, generator, cooling unit, GPU, NIC,
  switch or fabric was contacted. Power and cooling are integers in a claim;
  nothing actuates anything.
* **Capacity is consumed, never derived.** The library has no model of what a
  hall, a feed or a cooling loop can actually deliver. It records what the
  snapshot said and holds commitments against it.
* **The persisted document is not a public interchange format.** `docs/FORMAT.md`
  documents it because operators need to inspect it, but it is versioned
  internally and its compatibility is governed by `kStateFormatVersion`, not by
  an external specification.
* **The attempt index is bounded and evicting.** An evicted attempt is no longer
  replayable, and a retry of one is reported as a conflict rather than applied
  twice. The bound is explicit (`LedgerOptions::max_attempts`, default 4096).
* **A missing head pointer is unrecoverable by design.** There is no automatic
  repair, because the only repair available would promote an uncommitted state.
  Recovery is an operator action.

### Sanitizer configuration

`FACILITY_CAPACITY_RESERVATION_ENABLE_ASAN=ON` adds `/fsanitize=address` for MSVC
and `-fsanitize=address,undefined` for GCC and Clang, on every target including
the tests, the CLI, the examples and the benchmarks. The address sanitizer was
run; the undefined-behaviour sanitizer was not, for the toolchain reason given
above.

## 14. Documentation map

* `README.md` — this file: boundary, model, proofs, limitations.
* `docs/FORMAT.md` — the canonical capacity-snapshot and state-document formats.
* `NOTICE` — the DCCP position of this repository and its third-party position.
* `CONTRIBUTING.md` — licensing of contributions, build and quality expectations.
* `include/dccp/facility_capacity_reservation/*.hpp` — the public contract, in
  comments attached to the declarations they govern.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
