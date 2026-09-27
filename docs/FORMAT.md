# Canonical document formats

Facility Capacity Reservation has two canonical text formats: the **capacity
snapshot** it consumes, and the **state document** it persists. Both are line
oriented, fully framed, positionally decoded and sealed with a trailing SHA-256
digest over every preceding byte.

Three properties matter throughout:

* **Deterministic.** Two runs that reach the same state produce byte-identical
  documents. The test suite asserts this directly.
* **Strictly decoded.** The decoder is stricter than the encoder: unknown keys,
  reordered keys, wrong field counts, non-canonical integers, unsorted or
  duplicated collections, a mismatched digest and any trailing byte are all
  refused.
* **Bounded.** Every count is checked against a configured bound before anything
  is allocated, and every document is size-checked before it is read.

Field values never contain `|`, `=`, a newline, a carriage return or a NUL: free
text is percent-encoded, and identifiers are restricted to `[A-Za-z0-9._-]`
starting with an alphanumeric character. That is what makes the framing
unambiguous.

---

## 1. Capacity snapshot

A snapshot is the capacity evidence this library consumes. It is produced by the
facility capacity runtimes; this repository neither derives it nor writes it in
production. The format exists so that capacity can be imported, inspected and
attributed to an exact generation.

### Layout

```
fcr-capacity-snapshot 1
ref=<SnapshotRef>
source-generation=<u64>
facility=<FacilityRef>
captured-at-tick=<u64>
pools=<n>
pool=<kind>|<scope>|<gross>|<withdrawn>|<floor>          (exactly n times)
end
digest=sha256:<64 lowercase hex digits>
```

| Field | Meaning |
| --- | --- |
| `ref` | identity of the snapshot document; opaque to this library |
| `source-generation` | the producer's generation; the exact value every operation preconditions on |
| `facility` | opaque reference to the facility the snapshot describes |
| `captured-at-tick` | the producer's capture tick, in the same logical tick domain as validity intervals |
| `pools` | number of pool records that must follow |
| `pool` | one capacity pool |
| `end` | terminator of the pool block |
| `digest` | SHA-256 of every byte before this line |

`kind` is one of `space`, `rack`, `power`, `cooling`, `facility-service`.
`scope` is an opaque scope reference. `gross`, `withdrawn` and `floor` are
canonical unsigned decimals.

Records **must** be in canonical order: by `kind` in the order above, then by
`scope` byte-wise. Duplicates are refused.

### Derived quantities

```
available  = gross - withdrawn        (checked subtraction)
reservable = available - floor        (checked subtraction)
```

Neither is stored. A pool whose `withdrawn` exceeds its `gross`, or whose `floor`
exceeds its `available`, is refused as `SNAPSHOT_INCONSISTENT` — at parse time and
again when it is installed.

### Example

```
fcr-capacity-snapshot 1
ref=snap-0001
source-generation=7
facility=site-alpha
captured-at-tick=1700000000000
pools=3
pool=space|hall-a|400|0|0
pool=rack|hall-a|40|4|6
pool=power|feed-a|2000000|0|100000
end
digest=sha256:8f14e45fceea167a5a36dedd4bea2543...
```

---

## 2. State document

A state document is the complete authoritative state of one reservation ledger.
It is written to `generations/state-<revision>-<digest16>.fcr`, where
`<digest16>` is the first 16 hexadecimal digits of the document's own digest, and
is made current by replacing the sealed head pointer.

### Layout

```
fcr-state 1
incarnation=<32 lowercase hex digits>
revision=<u64>
last-tick=<u64>
capacity=<0|1>
snapshot-ref=<ref|->
snapshot-source-generation=<u64>
snapshot-facility=<ref|->
snapshot-tick=<u64>
snapshot-digest=<64 hex digits|->
pools=<n>
pool=<kind>|<scope>|<gross>|<withdrawn>|<floor>          (exactly n times)
reservations=<n>
reservation.begin
  ...  (the fields below, in exactly this order)
reservation.end
attempts=<n>
attempt=<id>|<op>|<subject|->|<intent-digest>|<revision>|<epoch>|<generation>|<previous-source-generation>|<tick>
end
digest=sha256:<64 lowercase hex digits>
```

`capacity=0` means no capacity evidence is installed; in that case
`snapshot-ref`, `snapshot-facility` and `snapshot-digest` are `-` and `pools` is
`0`. `-` is not a legal identifier, so it can never collide with a present value.

### Reservation record

| Field | Notes |
| --- | --- |
| `reservation.id` | the identity, stable across amendments |
| `reservation.generation` | 1 for the original; every amendment mints the next |
| `reservation.authority-epoch` | the epoch that minted this generation |
| `reservation.revision` | the state revision that made it current |
| `reservation.last-attempt` | the attempt that produced this generation |
| `reservation.claimant` / `.tenant` / `.service` / `.priority` | opaque references |
| `reservation.headroom` | `guaranteed`, `firm` or `opportunistic` |
| `reservation.source-snapshot` | the snapshot the commitment was priced against |
| `reservation.source-generation` | the source generation it was priced against |
| `reservation.validity` | `start|deadline`, half-open: `start <= t < deadline` |
| `reservation.state` | `active`, `released`, `expired` or `revoked` |
| `reservation.created-at-tick` / `.updated-at-tick` | logical ticks |
| `reservation.claims` | number of `claim=` lines that must follow |
| `claim` | `kind|scope|amount`, amount non-zero, canonical order, unique pool |
| `reservation.lineage` | number of `lineage=` lines that must follow |
| `lineage` | `generation|predecessor|attempt|revision|epoch|tick|cause|detail` |
| `reservation.termination` | `0` or `1` |
| `termination` | `cause|actor|-|attempt|revision|epoch|tick|policy|-|detail` |

Lineage entries chain contiguously: the first entry's `predecessor` is generation
1 and each subsequent entry's `predecessor` is the previous entry's `generation`;
the record's own `generation` is one past the last entry. A chain that skips or
repeats a generation is refused.

An active record carries no termination provenance, and a terminal record must
carry it; both directions are checked.

`cause` tokens for `termination` are `claimant-request`, `deadline-elapsed`,
`authority-revocation`, `capacity-withdrawn`, `superseded`, `facility-override`.
`cause` tokens for `lineage` are `correction`, `capacity-increase`,
`capacity-decrease`, `scope-change`, `deadline-extension`, `deadline-reduction`,
`priority-change`, `headroom-change`, `other`.

### Attempt record

| Field | Notes |
| --- | --- |
| `id` | the attempt identity |
| `op` | `reserve`, `amend`, `release`, `expire`, `revoke` or `reconcile` |
| `subject` | the reservation addressed, or `-` for sweeps and reconciliation |
| `intent-digest` | SHA-256 of the request's intent, length-prefixed and unambiguous |
| `revision` / `epoch` | the state the attempt produced |
| `generation` | the resulting generation where one is addressed, else `0` |
| `previous-source-generation` | reconciliation only |
| `tick` | the logical tick the attempt observed |

Records appear in eviction order (oldest first), which is the order the index
evicts them in when it exceeds its bound.

### Percent encoding

Free text — amendment and termination details — is percent-encoded: bytes in
`[A-Za-z0-9._:/-]` are emitted verbatim and every other byte becomes `%XX` with
uppercase hexadecimal digits. The encoding is injective, and a decoder refuses
both a malformed escape and an escape of a byte that must appear verbatim.

---

## 3. Sealed companion documents

`fcr.meta` and `fcr.head` use the same sealing rule: a signature line, one
`key=value` line per field, and a final `digest=sha256:<hex>` line over every
preceding byte.

```
fcr-store 1
incarnation=<32 hex digits>
digest=sha256:<64 hex digits>
```

```
fcr-head 1
incarnation=<32 hex digits>
head-sequence=<u64>
revision=<u64>
epoch=<u64>
state-file=state-<revision>-<digest16>.fcr
state-digest=<64 hex digits>
state-bytes=<u64>
digest=sha256:<64 hex digits>
```

`head-sequence` increases by one on every head write, including the epoch
handover that `open()` performs. `state-file` must be a single path component
matching `state-<canonical-u64>-<16 lowercase hex>.fcr`; anything containing a
separator, a drive qualifier, a control byte, `..` or `.` is refused with
`PATH_ESCAPES_ROOT`. A repeated key is refused rather than resolved by first
match, and a missing key is refused.

---

## 4. Integrity

The trailing digest covers every byte that precedes it, including the newline
that ends the previous line. It is computed before the digest line is appended,
so the verification path — which hashes everything before the final line — sees
exactly the bytes that were hashed.

The digest detects accidental corruption, truncation and substitution. It is not
a signature and does not authenticate the writer: an attacker who can rewrite the
store can rewrite the digest with it.
