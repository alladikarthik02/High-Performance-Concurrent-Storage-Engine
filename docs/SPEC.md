# `lsmeng` — Technical Specification

**Project: "High-Performance Concurrent Storage Engine".**

**Version 2.** Version 1 was attacked from six adversarial angles — durability,
concurrency, LSM design, scope/API, testability, and claim honesty. 36 defects were
raised, 23 were refuted on a careful re-read, and **21 survived and are fixed in this
document**. §11 records every one, with the v1 text that was wrong. Four of them were
silent-data-loss bugs.

Status: specification complete, implementation not started.

---

## 0. How to read this document

This is the document to re-read before a design review, and after finishing every task.
It has four jobs:

1. **§1–§3** say what is being built and how it works, in enough detail that the design
   can be defended out loud without looking at code.
2. **§4** is the required API surface. If a function is listed there and does not exist
   at the end, the project is not done.
3. **§5** is a numbered safety checklist. After each task, walk it top to bottom and
   confirm every item still holds. Items are referred to as `S1`, `S2`, …
4. **§6** is the edge-case inventory — failure modes written down *before* they are hit,
   so that when one shows up in a test it is recognised rather than discovered.

**§9** is the task breakdown. **§10** is the benchmark/profiling plan. **§11** is the
review log: what the attack found, and what changed.

A convention borrowed from `wanrep`: **every number here that is a claim about the real
world is either `MEASURED` (naming the task that measured it) or `ASSUMED` (a hypothesis
a task must confirm or kill).** Nothing in between. An unmarked number is a bug in this
document.

---

## 1. What we are building, and what we are deliberately NOT building

### 1.1 The thing

An **embeddable, persistent, ordered key–value storage engine in C++20**, built from
scratch, whose on-disk structure is a **log-structured merge tree**.

"From scratch" is the point of this project and is non-negotiable:

| Component | Who writes it |
|---|---|
| Write-ahead log, framing, recovery | us |
| Memtable (skip list) and its arena | us |
| SST file format: blocks, index, footer, checksums | us |
| Bloom filter | us |
| Compaction (tiered) | us |
| MANIFEST / version set / crash-consistent file bookkeeping | us |
| Block cache (sharded LRU) and table cache | us |
| CRC32C, varint, comparators, hashing | us |
| Benchmark harness and latency histogram | us |

**Zero third-party libraries in the engine or its tests.** No RocksDB, no LevelDB, no
Snappy, no gtest, no Abseil. The only things linked are libstdc++ and pthreads.

The reason is not purity. The claim says *"Built a C++ storage engine using an
LSM-tree design with a write-ahead log … Bloom filters and a tiered compaction
strategy."* If those four things belong to a library, that sentence is not true, and an
reviewer who asks "what is your SST block format?" gets an answer about somebody
else's code. **This project exists so that the answer is ours.**

This is not hypothetical. The earlier `Key_Value_Store_project_1` was a 66-line
pass-through over RocksDB whose entire Bloom filter was one call to
`NewBloomFilterPolicy(10)`; see `CHALLENGES.md` C1. That project is a gRPC/Raft project
and will be described as one.

### 1.2 What it is not

- **Not a database server.** No network, no gRPC. A library plus a CLI.
- **Not transactional across keys beyond a single batch.** `WriteBatch` is atomic; that
  is the whole transaction story.
- **Not a blob store.** Values are bounded (§3.1).
- **No secondary indexes.** Point lookups and ordered scans over keys.
- **No block compression in v1.** The format reserves a compression-type byte and the
  code path exists; the only registered type is `kNoCompression`.
- **Not multi-process.** One process per database directory (S16). Concurrency *within*
  that process is the entire concurrency story.
- **Not tuned for spinning disks.** I/O sizing assumes SSD/NVMe.

### 1.3 Success, stated as a sentence

*A single process with 8 writer threads and 8 reader threads can hammer this engine for
minutes on end, with compaction running the whole time, and: no reader ever sees a value
that was never written, no acknowledged durable write is lost across `kill -9`, the
engine never deadlocks, ASan/TSan/UBSan/helgrind stay silent, and we have p50/p99/p99.9
latency numbers plus a before/after profile showing one specific contention point that
was found with a profiler and removed.*

---

## 2. Claims → testable requirements (the contract)

### Claim 1

> *"Built a C++ storage engine using an LSM-tree design with a write-ahead log for
> durability, sustaining high write throughput while keeping reads fast through Bloom
> filters and a tiered compaction strategy."*

| # | Claim | Requirement | Verified by |
|---|---|---|---|
| R1 | "C++ storage engine" | Embeddable library — `Open/Put/Get/Delete/Write/NewIterator/Close` — with no external deps | T1–T9; links only libstdc++/pthread |
| R2 | "LSM-tree design" | Writes land in memory + log, flush to immutable sorted files, merge downward in the background. Never an in-place update of a data file | T5, T8, T10 |
| R3 | "write-ahead log for durability" | Every mutation is in the WAL with its CRC before it is acknowledged when `sync=true`; recovery replays it exactly, across **all live logs** | T4, T7, crash tests |
| R4 | "for durability" | After `kill -9` at an arbitrary point, reopen loses **no** write acknowledged with `sync=true`, and never returns a partially-applied `WriteBatch` | T4, T7, T8, `test_crash.cpp` |
| R5 | "sustaining high write throughput" | A **measured** ops/s figure with N threads, stated key/value sizes, sync mode, and hardware — **stated against a named baseline** (§2.1). Group commit demonstrated: `wal-syncs` ≪ `writes` at high thread counts | T8 (ratio), T11 (throughput) |
| R6 | "keeping reads fast through Bloom filters" | Own Bloom implementation; **measured** FPR vs theory; **measured** reduction in data blocks read per `Get`, with filter-block reads counted separately and honestly | T2, T11 |
| R7 | "a tiered compaction strategy" | Tiered compaction: the **T oldest** files of a tier merge into one file in the next tier, with tier count bounded by `max_tiers`. Read amplification **measured** as tiers grow, on an overwrite workload as well as an insert-only one | T10, T11 |

### Claim 2

> *"Optimized the concurrent path with fine-grained locking and benchmarked it under
> multithreaded load, profiling with perf and Valgrind to remove contention and reduce
> tail latency at scale."*

| # | Claim | Requirement | Verified by |
|---|---|---|---|
| R8 | "the concurrent path" | Concurrent readers + concurrent writers + background flush + background compaction, all live simultaneously | T8–T10, TSan suite |
| R9 | "fine-grained locking" | A written lock inventory (§3.10) with a documented order; **no lock held across file I/O**, enforced by a runtime assertion, not by inspection (S13); sharded block and table caches | T0 (TrackedMutex), T6, T8 |
| R10 | "benchmarked under multithreaded load" | Benchmarks parameterised by thread count, reporting throughput **and** p50/p90/p99/p99.9/max, closed- and open-loop | T11 |
| R11 | "profiling with perf and Valgrind" | Real profiler output committed. Where a tool cannot work in this environment, that is documented with the reason and the substitute used (§10.4) | T12 |
| R12 | "to remove contention" | At least one **specific, named** contention point, found by a profiler, with before/after numbers and the mechanism | T12 |
| R13 | "reduce tail latency at scale" | A **measured** p99/p99.9 improvement from a named change, at a stated thread count and a **fixed offered rate both builds sustain** (§10.1) | T12 |

### 2.1 The honesty rule, and the baselines

Every one of R5, R6, R7, R11, R12, R13 needs a number measured on this machine.
`docs/BENCHMARKS.md` holds them with the command line that produced each. A number
without its command line does not go in. **If a number cannot be produced, the claim
is rewritten — the claims follow the code.**

"High" and "fast" are comparatives and need a *comparand*, or a reviewer will supply
one. Three baselines, all measurable here:

- **B-null** — `write()` + `fsync()` of the same bytes to a flat file, no index, no
  structure. The floor: what the hardware and this container can do. Our write path
  cannot beat it; the claim is that we get *close* to it while providing an index,
  which is the actual point of an LSM tree.
- **B-selfscale** — this engine at 1 thread vs N threads. Group commit's whole promise
  is that this scales; a flat line is a failed claim.
- **B-selfconfig** — this engine against itself with a feature off: `bloom_bits_per_key=0`,
  `cache_shards=1`, `sync=true` vs `false`. This is where R6 and R12 actually live, and
  it is the honest kind of comparison — same code, one variable.

**We do not benchmark against RocksDB and claim victory.** A 2 kLOC engine beating a
mature one would mean the workload was rigged, and saying so unprompted is worth more in
a review than any number.

---

## 3. Architecture

### 3.0 The one idea

**Never modify a file that already exists on disk. Only ever append new files, and merge
old ones into new ones in the background.**

Everything follows. Because data files are immutable:

- a write is a memory insert plus a sequential log append — the two cheapest operations
  a machine offers, which is where "high write throughput" comes from;
- a reader can hold a file open with no lock while a compactor replaces it, because the
  compactor cannot mutate what the reader is looking at — only produce a *different*
  file and later delete the old one. That is where the concurrency story comes from;
- a crash can never leave a data file half-updated, only leave a file nobody references
  — that is where crash safety comes from.

The cost is read amplification: a key may live in the memtable or any SST. Bloom filters
and per-file key ranges make most of those touches free; compaction keeps the file count
small. That trade — cheap writes paid for with expensive reads, then bought back with
filters and merging — *is* the LSM tree.

```
   WRITE PATH                                 BACKGROUND                READ PATH
   ----------                                 ----------                ---------
   Put(k,v)                                                             Get(k)
      |                                    ONE background thread:          |
      v                                    flush first, else compact       v
  [ writer queue ] --group commit-->       shallowest eligible tier   [ memtable ]
      |                                              |                    | miss
      v                                              |                    v
  [   WAL append (+fsync)   ]                        |            [ immutable memtable ]
      |                                              |                    | miss
      v                                              |                    v
  [   memtable (skiplist)   ]                        |            for each tier, newest
      |                                              |             to oldest:
      | full  -> new log + new memtable              |             [ key range check ]
      v                                              |                    |
  [ immutable memtable ] ------- flush -----> [ tier 0 SSTs ]      [ bloom (in reader) ]
        ^                                            |                    |
        |                                     T oldest merge        [ index -> block ]
     two logs are live here                          v                    |
     (§3.3 log rotation)                       [ tier 1 SSTs ]      [ block cache ]
                                                     v              first match wins
                                               [ tier 2 SSTs ]
```

### 3.1 Data model, limits, and the internal key

Keys and values are arbitrary byte strings — **NUL bytes and non-UTF-8 bytes are legal**,
because a key is a `Slice` (pointer + length), never a C string.

| Limit | Value | Why |
|---|---|---|
| Max key length | 4 KiB | Must fit in a data block with its value; bounds the index entry size |
| Max value length | 1 MiB | Larger values wreck compaction and are the blob use case excluded in §1.2 |
| Max `WriteBatch` payload | 8 MiB | A batch is atomic, so it must fit in one WAL record (E-4) |
| Empty key | **legal** | It is the smallest key; scans start there |
| Empty value | legal | Distinct from absent — `Get` returns found-with-empty |

Exceeding a limit returns `Status::InvalidArgument`. Never truncated, never silently
accepted (S3).

**Sequence numbers.** Every mutation receives a globally unique, monotonically increasing
56-bit sequence number, **allocated from 1** (never 0 — see E-33). This one mechanism
provides ordering, snapshot isolation, and the rule for which duplicate wins in a merge.

**The internal key:**

```
internal_key := user_key || packed          (packed is 8 bytes, little-endian)
packed       := (seq << 8) | type
type         := 0x00 = kTypeDeletion (a tombstone)
                0x01 = kTypeValue
```

The comparator, written out because it is the crux of the design:

```
compare(a, b):
    c = bytewise_compare(a.user_key, b.user_key)   # shorter-is-smaller on a prefix tie
    if c != 0: return c
    # SAME user key: newer (larger seq) sorts FIRST
    return (b.packed < a.packed) ? -1 : (b.packed > a.packed) ? 1 : 0
```

Two consequences worth saying out loud:

1. All versions of one user key are **adjacent, newest first**. A `Get` is a single
   `Seek(user_key || (seq<<8|0xFF))` and then "take the first entry whose user key
   matches" — no backwards scan, no second comparison pass.
2. For an identical `seq`, `kTypeValue` would sort before `kTypeDeletion`. **That case
   cannot arise** because sequence numbers are unique — but the code must not *depend*
   on the accident, so the test suite asserts uniqueness rather than assuming it (S9).

The same comparator is used by the memtable, block builder, merging iterator, and
compaction. If any two disagreed the database would return wrong answers rather than
crash, so it is one function in one header, brute-forced for strict weak ordering over a
generated corpus (S10, E-16, E-17).

### 3.2 The write path and group commit

**The problem.** With `sync=true` a write costs an `fsync` — 50 µs to several ms. Eight
threads each doing their own `fsync`, serialised on one log file, gives a system whose
write throughput *does not improve with more threads*.

**The fix: leader/follower group commit.** One `fsync` commits many writers' data.

```
Put()/Write(batch):
  1. Wrap the batch in Writer { batch, sync, done=false, cv }.
  2. lock(db_mutex_); writers_.push_back(&w);
  3. while (!w.done && &w != writers_.front()) w.cv.wait(db_mutex_);
  4. if (w.done) { unlock; return w.status; }        // a leader did our work
  5. --- LEADER, holding db_mutex_ ---
     MakeRoomForWrite():  may switch memtable, may wait for flush.  NO I/O. (§3.2.1)
       -- its wait has an escape predicate covering bg_error_ and shutting_down_ (S22)
     Build a merged batch from writers_.front() .. up to a size cap,
       stopping at the first writer whose `sync` differs from ours.   (S2)
     Assign the sequence range [last_sequence_+1, ...].
  6. unlock(db_mutex_);
     --- NO LOCK HELD: the expensive part ---
     wal_->AddRecord(merged);     if (sync) wal_->Sync();
     merged.InsertInto(memtable_);       // single-writer: only a leader does this
  7. lock(db_mutex_);
     last_sequence_ += merged.count;
     pop the batched writers; set done=true and status; notify each cv;
     notify the new front. unlock.
```

**Why releasing the mutex at step 6 is safe.** Two invariants:

- **Only the leader writes.** A thread appends to the WAL or inserts into the memtable
  only while it is `writers_.front()`. There is exactly one front. So the WAL has one
  writer and the memtable has one writer, without either needing a lock. That is why the
  skip list can be lock-free-for-readers with no writer lock (§3.4).
- **Nothing else may swap the memtable.** The memtable pointer changes only in
  `MakeRoomForWrite`, at step 5, under the mutex, by the leader. The background thread
  only *reads* the immutable memtable. So the pointer captured at step 5 is still valid
  at step 6. Fragile, therefore asserted: `MemTable::Add` has a debug-only
  `assert(caller_is_current_leader)` (S12, E-21).

**Durability of the group.** Batching a `sync=false` writer into a `sync=true` group is
harmless. Batching a `sync=true` writer into a group the leader will *not* fsync is a
durability bug. Hence step 5 stops the batch at the first differing `sync` flag (S2).

**Expected result, `ASSUMED` until T8/T11:** `wal-syncs`/s stays roughly flat from 1 to
16 threads while writes/s climbs. If that graph does not appear, group commit is not
working and R5 is not earned.

**`MEASURED` in T11 — it appears.** From 1 to 16 threads with every writer asking
`sync=true`: throughput 3,031 → 20,950 ops/s (**6.9×**) while the fsync count *falls*
40,000 → 4,718 (**8.5×**), i.e. **1.00 → 8.48 writes per fsync**. At one thread the engine
sustains 99.6% of a raw-`write`-plus-`fsync` floor while maintaining a full sorted index.

The counterpoint is equally measured and worth volunteering: with `sync=false`, throughput
*falls* with thread count (1.05M → 169k, 1 → 8 threads). Group commit amortises an expensive
operation; where there is none to amortise, the serialisation is pure cost.

#### 3.2.1 `MakeRoomForWrite` performs no I/O — and why that needed saying

A memtable switch needs a **new log file**, and creating one is `open(O_CREAT)` plus an
`fsync` of the directory (E-7). Doing that inside `MakeRoomForWrite` would hold
`db_mutex_` across an `fsync` — blocking every queued writer *and* every `Get` (which
takes the mutex at §3.7 step 1) behind a several-millisecond directory sync, landing
squarely in the p99.9 metric R13 claims to improve. It would also violate S13, which v1
asserted with no exception listed. So:

- **The next log is pre-created.** Whenever a memtable is frozen, the background thread
  creates log `N+1`, writes and fsyncs its header, and fsyncs the directory *before* it
  is needed. A switch is then a swap of two already-open handles under the mutex.
- **If no pre-created log is available** (first open, or a switch that outran the
  background thread), the leader sets `need_log_`, **releases `db_mutex_`**, creates and
  fsyncs the log, re-acquires, and re-checks. It never creates a file with the mutex held.

Enforced mechanically, not by inspection: in debug and sanitizer builds `db_mutex_` is a
`TrackedMutex` that sets a bit in a thread-local mask, and **every `Env` entry point that
can block on the filesystem begins with `assert(!(held_locks & kDbMutex))`** (S13). Zero
cost in release builds.

### 3.3 The WAL: format, rotation, and torn-tail recovery

**File layout.** `NNNNNN.log`.

```
Header (16 bytes, once per file):
   [ magic      : 8 ]   "LSMWAL\x00\x01"
   [ log_number : 8 ]   little-endian, never 0

Record (repeated to EOF):
   [ crc32c  : 4 ]   over ( len || type || payload ), SEEDED with log_number
   [ len     : 4 ]   payload length, little-endian, MUST be >= 1
   [ type    : 1 ]   0x01 = kFullBatch
   [ payload : len ]
```

Payload = a serialised `WriteBatch`:

```
   [ seq : 8 ][ count : 4 ]
   repeated count times:
     [ type : 1 ][ klen : varint ][ key ][ vlen : varint ][ value ]
                                         (vlen/value absent for kTypeDeletion)
```

**Recovery of one file** reads records front to back, verifies each CRC, replays each
batch. On the *first* record failing any check, replay of **that file** stops and the
file is logically truncated there. Four checks:

1. Fewer than 9 bytes remain → torn header → stop.
2. `len == 0` → **invalid by construction** → stop.
3. Fewer than `len` bytes remain → torn payload → stop.
4. CRC mismatch → stop.

**Why `len == 0` is banned.** `CRC32C("")` is `0x00000000`, so `crc=0, len=0, type=0` is
self-consistent — and a zero-filled region is exactly what many filesystems produce for a
preallocated-but-unwritten extent. A zero hole would parse as infinitely many valid empty
records. Two independent defences: zero length is rejected, and the CRC is seeded with the
file's non-zero `log_number` so an all-zero header cannot match its own checksum. Belt
and braces, because this failure is silent (S4, E-5).

#### 3.3.1 Log rotation and the live log set

**This is the part v1 got wrong** (§11.1). A memtable and a log file are 1:1, so the
moment a memtable is frozen there are **two live logs**, and recovery must replay both.

- **Rotation.** When `MakeRoomForWrite` freezes the mutable memtable, it swaps to log
  `N+1` (pre-created per §3.2.1). The old log stays open and undeleted until the flush of
  its memtable has a durable MANIFEST edit.
- **Manifest fields.** `VersionEdit` carries `log_number` (the log feeding the *mutable*
  memtable) and `prev_log_number` (the log feeding the *immutable* memtable, or 0).
  Define `min_live_log = prev_log_number ? min(log_number, prev_log_number) : log_number`.
- **Recovery** replays every `NNNNNN.log` with `NNNNNN >= min_live_log`, **in ascending
  numeric order**, into one stream. Ordering by file number is correct because later logs
  carry higher sequence numbers. **The torn-tail rule is per file:** a truncated log stops
  replay *of that file only*; replay continues with the next. A truncated earlier log
  followed by an intact later one is a normal state (an un-fsynced `sync=false` tail in
  the old log, fsynced records in the new one) and must not stop recovery.
- **GC.** `RemoveObsoleteFiles` keeps the SSTs referenced by all live versions, **every
  log with number ≥ `min_live_log`, and the manifest named by `CURRENT`**; the rest are
  candidates (E-6 governs whether they are deleted).

**Ordering rule that makes R4 true.** For an acknowledged `sync=true` write the bytes are
in the WAL and `fsync`ed **before** the acknowledgement, and before the memtable insert is
visible. Step 6 does WAL → fsync → memtable, which is the correct order. The reverse would
let a reader observe a value a crash can then erase (S1).

**What "durable" means here, honestly.** `fsync` inside a Linux container on Docker
Desktop for macOS crosses a virtio/gRPC-FUSE boundary into a VM and then APFS. That chain
is **not** a power-loss guarantee and this project will not claim it is. Our crash tests
verify *process* crash-consistency via `kill -9` (real, sufficient for framing, ordering
and replay) and, separately, verify the un-fsynced-data case through the `Env` crash
simulation, which drops writes the page cache would otherwise have kept (§7 layer 4,
E-14). The distinction is stated here so it is never overstated in conversation.

### 3.4 The memtable: a single-writer skip list

**Why a skip list, not `std::map`:**

1. **Insert allocates new nodes and touches only forward pointers.** A concurrent reader
   can never observe a partially-rebalanced structure, because there is no rebalancing.
   A red-black tree rotates, and a rotation is exactly the transient inconsistency that
   would force readers to lock.
2. Ordered iteration is free, which the merging iterator and the flush both need.
3. Random node heights pair naturally with an arena.

**Structure.** `kMaxHeight = 12`, branching factor 4 (~16M entries). Encoded internal
keys stored inline, allocated from an arena.

**Concurrency contract.**
- **Exactly one writer at a time**, guaranteed externally by the group-commit leader
  invariant (§3.2). The skip list takes **no lock at all**.
- **Any number of concurrent readers**, no lock.
- Forward pointers are `std::atomic<Node*>`. The writer publishes with `store(release)`
  bottom level upward; readers `load(acquire)`. That pair is what guarantees a reader who
  sees the pointer also sees the fully-initialised key bytes. **This is the most important
  memory-ordering fact in the project**, and precisely what TSan is being run to check (S6).
- Bottom level linked first. A reader that sees a node at level 3 but not yet level 5
  still finds it — search always descends to level 0.
- **Nothing is ever deleted from a skip list.** A memtable is filled, frozen, flushed, and
  destroyed whole. No node reclamation problem, so no hazard pointers and no RCU. Saying
  *why we don't need them* is stronger than using them.

**Arena.** 4 KiB blocks, bump allocation, no per-node free. Usage tracked in an
`atomic<size_t>` so `MakeRoomForWrite` can compare against `write_buffer_size`. The number
tracked is **arena bytes, not user bytes** — the difference is node headers, height
pointers and alignment. **`MEASURED` in T3: 1.28×** for 16 B keys and 100 B values, not the
2–3× this spec originally assumed. So a 4 MiB `write_buffer_size` holds ~3.1 MiB of user
data and flushes roughly half as often as the pessimistic estimate predicted — which
matters, because flush frequency drives tier-0 pressure and therefore the write stalls of
§3.8.4.

### 3.5 The SST file format

Immutable, written once, sequentially, never modified. Named `NNNNNN.sst`.

```
  +-------------------------------+
  |  data block 0                 |   ~4 KiB target
  |  ...                          |
  |  data block N-1               |
  +-------------------------------+
  |  bloom filter block           |   one filter for the whole file
  +-------------------------------+
  |  index block                  |   one entry per data block
  +-------------------------------+
  |  footer (48 bytes, fixed)     |
  +-------------------------------+
```

**Every block carries a 5-byte trailer:**

```
   [ compression_type : 1 ]   0x00 = none (the only value in v1)
   [ crc32c           : 4 ]   over ( block_contents || compression_type )
```

A block is never used without CRC verification (S7). Including the type byte in the CRC
prevents a "valid block, wrong interpretation" case where a flipped type byte routes good
bytes into a decompressor.

**Data block, with prefix compression.** Adjacent sorted keys usually share a prefix
(`user:1001:name`, `user:1001:email`); storing the shared part once is nearly free and
materially shrinks the file.

```
   entry := [ shared : varint ][ non_shared : varint ][ vlen : varint ]
            [ key_delta : non_shared bytes ][ value : vlen bytes ]
   ... entries ...
   [ restart[0] : u32 ] ... [ restart[R-1] : u32 ][ R : u32 ]
```

Every 16th entry is a **restart point** storing its key in full (`shared = 0`). The
restart array is what makes a block *searchable*: binary search over restart points lands
within 16 entries, then a short linear scan finishes. Without it, prefix compression would
force every lookup to decode from byte zero.

**The invariant:** an entry may only reuse a prefix of the *immediately preceding* entry,
and the first entry after a restart must set `shared = 0`. A builder bug here produces a
file that reads back with subtly wrong keys and **no checksum failure**, because the bytes
are internally consistent. So T5's round-trip test compares a full scan against the exact
input vector, not just spot lookups (E-8).

**Index block.** Same encoding. One entry per data block:

```
   key   = a separator S with   last_key(block_i) <= S < first_key(block_i+1)
   value = BlockHandle { offset : varint, size : varint }
```

The *shortest* such separator keeps the index small. Getting the `<=`/`<` boundary wrong
by one makes exactly the keys on a block boundary go missing — a bug random testing finds
and hand-written tests usually do not (E-9).

**Bloom filter block.** §3.6.

**Footer — fixed 48 bytes, read by seeking to `file_size - 48`:**

```
   [ bloom_handle : varint offset, varint size ]
   [ index_handle : varint offset, varint size ]
   [ zero padding to 40 bytes ]
   [ magic : 8 ]   0x6C736D656E673031  ("lsmeng01")
```

Magic last: an interrupted write cannot produce a footer that both parses and points at
garbage, and the file is linked into the MANIFEST only after `fsync`.

**Per-file metadata** (`smallest_key`, `largest_key`, `num_entries`, `num_deletions`,
`file_size`) lives in the **MANIFEST**, not the SST, because the version set needs it to
answer "can this file possibly contain key k?" **without opening the file at all**. It is
the cheapest read-path filter and runs before everything else — including, now, before the
Bloom filter (§3.7).

**Filter residency.** A file's filter block is read **once, when its `SstReader` is
opened**, and held for that reader's lifetime. It is *not* fetched through the block
cache: a whole-file filter is far larger than a block and would evict the very data blocks
it exists to protect. A deep-tier file can hold ~4³ flushed memtables ≈ 10⁶ keys, so at 10
bits/key its filter is ~1.25 MiB — 15% of the default 8 MiB block cache, for one file. So
the filter is **live memory attached to an open file, not cache**, and the table cache must
be bounded in **bytes** as well as file count (S11, E-34).

### 3.6 Bloom filters

**Purpose.** Answer *"is this key definitely not in this file?"* without reading a data
block. A "no" is certain; a "yes" is probabilistic. A false positive costs one wasted block
read — the situation we were in without a filter. A false *negative* would return wrong
data, and cannot happen. That asymmetry is the whole safety argument.

**Parameters.** `bits_per_key = 10` (configurable; 0 disables). **`k = floor(m·ln 2) = 6`**,
clamped to `[1, 30]`. Theoretical FPR at m=10: 0.0084 for k=6, 0.0082 for k=7.

*Why floor and not round* (decided by measurement in T2, not by taste): k=7 buys a 3%
better false-positive rate for **17% more probes per lookup**, and those probes sit on the
read hot path. T2 measured 0.00836 at k=6 — within 2% of that row's theory. The clamp to
`[1, 30]` is load-bearing at the bottom: `bits_per_key = 1` would give k = 0 unclamped, and
a filter that probes zero bits answers "not present" for **everything** — a false-negative
machine, the one failure this design says is impossible.

**Hashing.** One 64-bit hash of the *user key* (not the internal key — the filter is
queried with a user key and must not depend on a sequence number), then
Kirsch–Mitzenmacher double hashing:

```
   h  = hash64(user_key)
   h1 = h & 0xFFFFFFFF ; h2 = h >> 32 ; h2 |= 1          # force odd
   for i in 0..k-1:  bit[ (h1 + i*h2) % nbits ] = 1
```

One real hash instead of seven, with an FPR indistinguishable from independent hashes at
these parameters. T2 **measures** it rather than trusting it.

**Block encoding.** `[ bit array : ceil(n·bits_per_key/8) bytes ][ k : 1 byte ]`. Storing
`k` in the file means a file written with different settings still reads correctly — the
reader never has to be configured to match the writer.

**Rules that are easy to get wrong:**

- **Tombstones go in the filter.** A `Delete` writes a `kTypeDeletion` entry and its key
  **must** be added. Omitting it means the filter says "not present", the file is skipped,
  and an older value in a deeper tier resurrects. The single worst bug available in this
  design (E-2a). Asserted in the SST builder: every entry added to the block is added to
  the filter, regardless of type.
- **Bloom filters cannot support removal.** Nothing ever clears a bit. Deletion is a
  tombstone record, never an un-set bit.
- **`n = 0`.** Minimum array size 8 bytes, all zero, so every probe hits a clear bit and
  answers "not present" — correct for an empty set. No division by zero, no zero-length
  allocation, no out-of-bounds probe (E-3).
- **A missing or corrupt filter must mean "maybe present", never "not present".** If the
  footer's bloom handle is empty or the block fails CRC, the reader falls back to reading
  the file. Failing *open* is the only safe direction (S8).

### 3.7 The read path

```
  1. lock(db_mutex_):
       seq = snapshot ? snapshot->seq : last_sequence_;   // captured HERE, under the lock
       mem->Ref();  if (imm) imm->Ref();  current_version->Ref();
     unlock(db_mutex_)
     -- from here on NO lock is held; everything is kept alive by refcount --
  2. lookup_key = user_key || ((seq << 8) | 0xFF)
  3. memtable.Get(lookup_key)   -> hit? return value, or NotFound if kTypeDeletion
  4. imm.Get(lookup_key)        -> hit? same
  5. for tier = 0 .. last_tier:
       for each file in tier, NEWEST FIRST (descending file number):
         if key < file.smallest_user_key or key > file.largest_user_key: continue  # no file touched
         reader = table_cache.Get(file)        # opens if not cached; its filter is read at open
         if not reader.bloom.MayContain(key): continue     # no DATA BLOCK read
         binary search index; read data block (block cache); search block
         if found: return value, or NotFound if kTypeDeletion
  6. return NotFound
```

**Why the sequence must be captured inside the same critical section as the refs.** v1 read
`last_sequence_` *before* taking `db_mutex_` (§11.5). In the gap, a compaction can install
a new Version that legitimately dropped the exact version this read needs, and the reader
then pins the *new* Version and misses — a silent stale read with no crash and no checksum
failure. Capturing both together fixes it, and the argument is one sentence worth being able
to give: *the pinned Version's file set is a superset of the files live at `seq`; files are
immutable and are not unlinked while a Version references them (S14); so every entry with
`seq' <= seq` that existed at capture time is still reachable through that pinned Version.*
`last_sequence_` is `std::atomic<uint64_t>` for stats readers, but **correctness comes from
the critical section, not the atomic** (S21). The same rule applies to `NewIterator` when
`ReadOptions::snapshot == nullptr` (§3.12).

**Why "newest first" is load-bearing, and the invariant it rests on.** Within a tier
produced by tiered compaction, files **overlap** — unlike leveled compaction's L1+, where
files are disjoint. So a tier is searched file-by-file, ordered by recency, and the first
hit wins. File number descending is the recency order *within* a tier. Across tiers the
argument is the **tier-ordering invariant** of §3.8, which is exactly why compaction inputs
must be an oldest-first prefix. Get this wrong and the database returns committed-but-stale
data forever, with no error anywhere (E-1).

**Cost model.** Per `Get` for an absent key: ~`0.008 × F` **data blocks** instead of `F`,
plus a one-time filter block read per file *open* (not per lookup). T11 reports
`lsmeng.filter-blocks-read` next to `lsmeng.blocks-read` so the filter's own I/O is visible
rather than hidden inside the win (R6).

### 3.8 Compaction: tiered

The claim says *tiered*, so this is tiered, not leveled, and the difference is something
to be able to explain:

| | Tiered (ours) | Leveled |
|---|---|---|
| Trigger | a tier holds ≥ T files | a level exceeds its byte budget |
| Inputs | the **T oldest** files of that tier | the overlapping files of two levels |
| Files within a level overlap? | yes | no (except L0) |
| Write amplification | **low** (~O(tiers)) | high (~O(10 × levels)) |
| Read amplification | **high** (every file in a tier may be checked) | low (one file per level) |
| Space amplification | high — measured steady state on an overwrite workload (T11); the transient ~2× of one compaction (E-27) is the smaller half of the cost |  low |

We chose the write-optimised end, coherent with a claim whose headline is write
throughput. "We picked the one with worse read amplification, and here is the number" is a
stronger answer than pretending there is no cost.

#### 3.8.1 The tier-ordering invariant — load-bearing for §3.7

> **For every user key, the shallowest tier containing it holds its newest version.**

*Proof by induction.* Tier 0 receives flushed memtables in flush order, so within tier 0
file number is recency. A compaction consumes the T **lowest-numbered (oldest)** files of
tier `t` and writes their merged contents to tier `t+1`; therefore everything in tier `t+1`
was written before everything remaining in tier `t`. So no version in tier `t+1` is newer
than any version of the same key in tier `t`, and §3.7's tier-ascending, file-number-
descending search returns the newest version. ∎

**This holds only because inputs are an oldest-first prefix of the tier.** A size-chosen or
newest-chosen subset breaks it and makes `Get` return stale data. v1 said the trigger was
"T *similarly-sized* files" — i.e. selection by size — which contradicts this proof and was
a real bug (§11.4).

#### 3.8.2 Selection, promotion, and the tier-count bound

- **Trigger.** Tier `t` is compacted when it holds ≥ `T = 4` files.
- **Inputs.** The **T lowest-numbered** files of that tier. Never a size-chosen subset,
  never the newest T. If the tier holds more than T (tier 0 routinely does — the stall
  triggers are 8 and 12), the rule is unchanged: still the T oldest, repeated.
- **Output.** One file in tier `t+1`. Checked after every flush and every compaction, so
  it cascades.
- **`max_tiers = 7`.** If a compaction would create tier `max_tiers`, it instead merges
  **every** file of the deepest tier together with the inputs into that same deepest tier.
  Consuming the whole tier keeps the invariant trivially (nothing older is left behind).

Files within a tier come out similarly sized **as a consequence** of this rule, not as a
selection criterion — an important distinction, because `Flush()`, recovery flushes (E-20),
and batch-boundary freezes all produce arbitrary-size tier-0 files, so "similarly sized" is
never something to rely on.

**Honest limitation, stated because a reviewer will find it.** This is count-triggered
tiering with an oldest-prefix rule. It is *not* Cassandra-style STCS, which buckets files by
size ratio and compacts a bucket wherever its members live. Under a workload that overwrites
a small key set forever, every merge output is about the size of its inputs, so equally
sized files accumulate in different tiers and this rule can never pair them; tier count then
grows with total bytes written rather than live data. `max_tiers` bounds the damage, and
T11's `tier_sweep` **measures** files-consulted-per-`Get` and tier count against total
flushes on an overwrite workload — reported, not hidden (E-32). Size-bucketed selection is
the named next step, and the reason it is not in v1 is that it would need a separate proof
that it preserves §3.8.1.

#### 3.8.3 The merge, and the two drop rules

A k-way merging iterator over the inputs using the §3.1 comparator, so for each user key the
newest version arrives first.

```
  last_user_key = <none>;  last_seq_for_key = kMaxSequence
  for each entry, in internal-key order:
      if user_key != last_user_key:
          last_user_key    = user_key
          last_seq_for_key = kMaxSequence          # nothing newer has been seen

      drop = (last_seq_for_key <= oldest_snapshot_seq)   # a NEWER version is visible to
                                                          # every live snapshot, so this
                                                          # one is unobservable

      if entry.type == kTypeDeletion
         and no_older_data_can_exist(user_key)
         and entry.seq <= oldest_snapshot_seq:
          drop = true

      last_seq_for_key = entry.seq                 # BEFORE the next iteration
      if !drop: emit(entry)
```

**Why the predicate is about the *previously emitted* version, not this one.** v1 wrote
`drop = (entry.seq <= oldest_snapshot_seq)`, which drops exactly the version an old snapshot
needs — the first version at or below the snapshot boundary always satisfies it (§11.3).
The correct statement: *an entry is unobservable only if the version that supersedes it is
itself visible to the oldest live snapshot.*

**`no_older_data_can_exist(user_key)`.** A tombstone may be discarded only when no file this
compaction is *not* rewriting could hold an older value. **`is_bottom_tier` alone is not
that test** — tiered compaction writes its output into a tier that already holds up to `T−1`
older, non-input files, and the merge never reads them (§11.2). The test is: for every file
in the output tier and every deeper tier that is not an input, check `user_key` against that
file's `[smallest_user_key, largest_user_key]`. Those ranges are already in `FileMetaData`,
so this is pure in-memory arithmetic with no I/O, and because the merge walks keys in
ascending order it is a forward cursor over those files sorted by `smallest_user_key` —
amortised O(1) per key.

*(Leveled compaction gets to skip this because it pulls the overlapping output-level files
into its input set. Tiered does not, which is why LevelDB's `IsBaseLevelForKey` shortcut
cannot be copied here. Being able to say that sentence is worth more than the code.)*

**`oldest_snapshot_seq`.** Captured **once**, under `db_mutex_`, when the compaction is
scheduled, and held constant for the whole merge:

```
   oldest_snapshot_seq = snapshots_.empty() ? last_sequence_ : snapshots_.front()->seq
```

With no live snapshot the value is `last_sequence_`, **not 0** — everything superseded is
droppable, which is what makes compaction reclaim space at all. It must not be re-read
mid-merge: a snapshot registered after capture gets a sequence above `last_sequence_` at
capture time, so it cannot need anything already dropped, whereas a re-read could lower the
threshold under a merge that already emitted on the old one. Sequence numbers start at 1, so
`0` is not a legal captured value and a debug assert rejects it — the accidental-zero bug
turns compaction into a pure merge that reclaims nothing **while every correctness test
still passes** (E-33).

#### 3.8.4 Concurrency, scheduling, and write stalls

**The thread model — exactly one background thread.** It loops: if an immutable memtable
exists, flush it; else if a tier is eligible, compact it; else wait on `bg_cv_`. "The flush
thread" and "the compaction thread" are two *jobs of one thread*. `Flush()` and
`CompactRange()` from a user thread schedule work and wait; they do no I/O themselves.

Three things fall out of that choice, and the third is the price:

1. **Only one thread ever executes the §3.9 append/install sequence**, so MANIFEST record
   order equals in-memory Version install order *by construction*, and no manifest lock is
   needed. v1 described two background threads with no lock serialising them — two
   unsynchronised appenders on one fd interleave bytes mid-record, the CRC fails at the
   splice, and §3.9's torn-tail rule then silently discards both edits and every later one
   (§11.6). A debug assertion records the manifest-appending thread id and fires if a second
   thread ever appends (S23).
2. Compaction reads only immutable files, writes a new file with a new number, `fsync`s it,
   and only then takes `db_mutex_` to install a new Version — pointer manipulation only,
   never I/O (S13).
3. **Head-of-line blocking, stated plainly rather than discovered later.** One compaction
   runs at a time; when several tiers are eligible the **shallowest** is picked, always,
   because tier-0 backlog is what stalls writers. A compaction in progress is never
   preempted. So while a deep-tier merge runs (hundreds of MiB, tens of seconds), tier 0
   cannot be drained, and a sustained write run can hit the stop trigger and block every
   writer for the rest of that merge. This is a known cost of the single-thread design, it
   is **observable** (`lsmeng.compaction-in-progress`, `lsmeng.max-stall-ms`,
   `lsmeng.pending-compaction-bytes`), and the named next step is a two-worker pool with the
   rule that no two compactions share an input file or target the same tier (E-31).

**Write stalls.** `kTier0SlowdownTrigger = 8` files → each write sleeps 1 ms, a deliberate
brake handing CPU to the compactor. `kTier0StopTrigger = 12` → writes block on `bg_cv_`
until tier 0 drops. Designed behaviour, not a bug, and visible in p99.9 — which is exactly
what R13 is about (E-11).

**Every wait on `bg_cv_` has an escape predicate** covering `bg_error_` and
`shutting_down_`, and every write to either is followed by `notify_all()` under
`db_mutex_`. A stalled writer must be woken by the **failure** of the thing it waits for,
not only by its success — otherwise a background thread that dies on `ENOSPC` leaves every
writer parked forever (S22, E-23). And because the stalling thread is the *leader*, holding
the leader slot, a missed wakeup is a whole-database outage, not one slow thread. On an
early return the leader must still run step 7 — pop itself and its batched writers, set
their status, notify them and the new front — or the queue is left with a corpse at its head
and the hang merely moves.

### 3.9 The MANIFEST, versions, and file lifetime

**The problem.** The set of live SST files *is* the database. Directory listing cannot
answer it: it cannot distinguish a live file from one left by a crash mid-compaction.

**The design:**

- A **`Version`** is an immutable, refcounted snapshot of the file set: per tier, an
  ordered list of `FileMetaData { number, size, smallest, largest, entries, deletions }`.
- A **`VersionEdit`** is a delta: files added, files deleted, plus `log_number`,
  `prev_log_number`, `next_file_number`, `last_sequence`.
- **`MANIFEST-NNNNNN`** is an append-only log of `VersionEdit`s using the **exact same
  record framing as the WAL** (§3.3) — same CRC, same torn-tail rule, same code. Reuse
  here is not laziness: it means the manifest's crash behaviour is tested by the WAL's
  tests.
- **`CURRENT`** is a tiny file naming the live manifest.

**Applying a change** — the ordering is the whole point:

```
   1. write the new SST                              (a new file number)
   2. fsync the SST
   3. append the VersionEdit to MANIFEST
   4. fsync the MANIFEST
   5. install the new Version in memory  (under db_mutex_)
   6. later: delete files no Version references
```

Exactly one thread is inside steps 3–5 at any time (§3.8.4 thread model). That is why
steps 3–4 can run with no lock held without violating S13, and why on-disk append order
always equals in-memory install order (S23).

Crash between any two steps is survivable: after (1) or (2), an orphan SST nothing
references. After (3) but before (4), the manifest's torn tail is truncated on replay, so
the edit did not happen and the SST is again an orphan. **Never the reverse order** —
recording a file before it is durable produces a manifest referencing a file that does not
exist, which is unrecoverable rather than merely untidy (S5, E-6).

**Rotating `CURRENT` atomically:**

```
   write CURRENT.tmp ; fsync(CURRENT.tmp) ; rename(CURRENT.tmp, CURRENT) ; fsync(dir)
```

`rename` is atomic on POSIX, so a reader sees the old name or the new one. **The `fsync`
of the containing directory is not optional** — without it the rename itself may not
survive a crash, leaving `CURRENT` naming a manifest that is gone. Directory fsync is the
classic omission in this pattern; it gets its own test (E-7, S5).

**Manifest growth and rotation.** Every flush and compaction appends an edit, so the file
grows forever and open time grows with it (E-13). At 64 MiB it is rotated — and **rotation
is a mode of applying an edit, not a separate activity**, which is the fix for a bug v1 had
(§11.7):

> The background thread, already inside steps 3–5 for the next edit, writes a fresh
> `MANIFEST-NNNNNN` containing a full snapshot of **the version it is about to install**
> (i.e. base + this edit), fsyncs it, rotates `CURRENT`, fsyncs the directory, and only
> then continues. **No edit is ever appended to a manifest after `CURRENT` has stopped
> naming it.** The superseded manifest is unlinked only **after** the `fsync(dir)`
> following the rename returns, and through the ordinary obsolete-file path — never an
> inline unlink, because a crash inside rotation would otherwise leave `CURRENT` naming
> the new manifest with the fallback already gone: E-7's unopenable database by another
> route.

v1 specified rotation in one sentence with no thread named and no exclusion. Because the
single-background-thread model (§3.8.4) makes the applier unique, rotation is safe by
construction here — but the *ordering* of the unlink still had to be stated (E-35).

**File lifetime — why compaction never pulls a file from under a reader.** A reader takes
`version->Ref()` under `db_mutex_`, then does all I/O with no lock. A compaction installs a
new `Version`; the old one lives until its refcount hits zero. `RemoveObsoleteFiles`
computes the union of files referenced by *all* live versions (plus the live logs and the
current manifest, §3.3.1) and considers the rest obsolete. Because files are immutable and
unlinking an open fd on POSIX leaves the fd usable, even a badly-timed unlink cannot corrupt
an in-flight read — the reader's descriptor keeps the inode alive until it closes. **That is
the safety argument for the entire concurrent read path, and it should be sayable in two
sentences** (S14).

**Orphan collection is opt-in, not automatic.** v1 deleted unreferenced files at every
`Open`, and justified it as safe "only because S16 guarantees a single process owns the
directory" — while S16 rests on `flock`, which `wanrep` already measured as **not
excluding** on this exact bind mount (its bug B2). Wiring an unconditional `unlink` loop to
a mechanism known to have failed here once is how you delete a live database. So
(§11.8, E-6):

- `Options::gc_orphans_on_open` defaults to **`false`**. The default path *reports* orphan
  files and their total bytes through `lsmeng.stats` and leaves them on disk.
- Collection is opt-in (`lsmeng_cli --gc`), and is **refused outright** in any build where
  T7's two-process exclusion test did not pass.
- An orphan SST wastes space; a wrongly unlinked live SST ends the database. Those are not
  the same class of problem, and the default should reflect that.

### 3.10 Concurrency: the lock inventory and the lock order

Direct evidence for R9. Exhaustive on purpose: the follow-up to "I used fine-grained
locking" is "what locks, in what order, and what did you avoid holding them across?"

**The thread model.** Foreground application threads (any number) + **one** background
thread (§3.8.4). That is all. There is no thread pool.

| # | Lock | Protects | Held across I/O? |
|---|---|---|---|
| L1 | `db_mutex_` (`std::mutex`) | Writer queue, memtable pointers, `versions_`, snapshot list, compaction state, file-number counter, obsolete-file set | **No** — enforced by assertion (S13) |
| L2 | `ShardedLRUCache` shard mutex ×16 | One shard's hash table + LRU list | **No.** The block is read outside, then inserted |
| L3 | `TableCache` shard mutex ×16 | Open `SstReader` handles (and their resident filters) | **No.** Same open-outside-then-insert pattern |
| L4 | per-`Writer` condition variable | One writer's `done` flag | n/a |
| L5 | `bg_cv_` | Background job state, stalls, shutdown | n/a — paired with L1 |

**Lock order: `L1 → (L2 | L3)`. Never the reverse.** Enforced by inspection *plus* a
debug-build lock-order tracker in `tests/lockorder.h`, plus TSan.

**There is deliberately no manifest lock.** With one background thread, MANIFEST appends
are serialised by construction, and adding a lock held across `fsync` would create a
documented exception to S13 for no benefit. The invariant is asserted instead: the manifest
writer records its thread id and a second appender trips an assert (S23). *This was a
design decision, not an omission — the alternative (two background threads plus an `L6
manifest_mutex_` that is knowingly held across I/O) was considered and rejected because one
thread removes the race rather than guarding it. The cost is the head-of-line blocking in
§3.8.4, which is measured and reported rather than hidden.*

**Deliberately not locked, with the reason:**

- **The skip list.** Single-writer by the group-commit invariant; readers use
  acquire/release (§3.4).
- **A `Version`.** Immutable after construction; only its refcount is atomic.
- **SST files.** Immutable. Concurrent `pread` on one fd from many threads is safe because
  `pread` does not touch the file offset. Using `read` here would be a race on the shared
  offset, producing interleaved garbage that *looks like* an SST checksum bug — so the
  `Env` interface exposes only positional reads and there is no API to make the mistake
  (E-25).

**The known, accepted race: cache-miss stampede.** Two threads missing on the same block
both read it; one insert wins. Deliberate — the alternative is holding a shard mutex across
a disk read, serialising every reader behind the slowest I/O. Cost: a rare duplicate read.
Benefit: L2 is never held during I/O. Documented, not accidental (E-12).

**Where the contention will be** — the hypothesis T12 exists to test: `db_mutex_` on the
read path (every `Get` takes it at §3.7 step 1) and the block cache shards.

**`MEASURED` in T12, and the hypothesis was half wrong.** Sharding the block cache is worth
1.58× throughput and 2.9× at p90, so that half stands. But in the configuration deliberately
rigged to be lock-bound, `perf` attributed **13.7% of cycles to allocation against 8.0% to
lock machinery** — a larger cost this section did not anticipate, caused by a `std::string`
built *inside* the shard mutex on every cache lookup plus three heap iterators per point
lookup. Both are fixed (+41% throughput, p90 −29%, run-to-run spread 24% → 1.4%), and the
bottleneck has moved to iterator allocation on the *scan* path. `BENCHMARKS.md` §T12,
`CHALLENGES.md` B21.

### 3.11 The block cache

A **sharded LRU**: 16 independent shards, each `{ mutex, hash table, LRU list, capacity/16 }`,
shard chosen by `hash(file_number, block_offset)`.

Sharding is the point. An unsharded LRU serialises *every* block access in the process
behind one mutex — and unlike a structure that is merely read, an LRU is **mutated on every
hit** (the entry moves to the list head), so even a pure-read workload contends. This is the
concrete, measurable "removed contention" story for R12: T12 benchmarks 1 vs 4 vs 16 shards
and reports the delta.

**`MEASURED` in T11/T12: it matters.** 1 → 16 shards at 8 threads gives **1.58× throughput
and p90 from 29 µs to 10 µs**. Note what did *not* move: **p99.9 is unchanged (240 → 295 µs)**,
so the deep tail is not the cache lock — under the mixed workload it is compaction, and that
negative result is reported rather than buried (the `wanrep` precedent: its "lock-free beats
a mutex" hypothesis was measured and killed).

Entries are refcounted so a block in use is not evicted under its reader.

The **table cache** is the same structure holding open `SstReader`s, bounded by whichever of
`max_open_files` (a count, for fds — E-26) or `filter_memory_bytes` (bytes, for resident
filters — §3.5, E-34) binds first.

### 3.12 Iterators and snapshots

**`Snapshot`** is a sequence number plus a registration in a sorted list under `db_mutex_`.
Compaction reads `oldest_snapshot_seq` from its front, once, at schedule time (§3.8.3). A
leaked snapshot therefore **stops space reclamation forever** — so `SnapshotHandle` is the
RAII wrapper every test and benchmark must use, `~DB` asserts the list is empty in debug
builds, and `lsmeng.stats` reports the oldest live snapshot's age (E-10). The raw
`GetSnapshot`/`ReleaseSnapshot` pair remains for C-style embedders.

**`NewIterator`** returns a merging iterator over the mutable memtable, the immutable
memtable, and every SST in every tier, each pinned by refcount for the iterator's lifetime.
When `ReadOptions::snapshot == nullptr`, the sequence and the refs are captured in **one**
`db_mutex_` critical section, exactly as in §3.7 (S21). A long scan pins files against
deletion — a space cost, and the reason iterators should be short-lived; stated in the API
contract.

The merge is a binary heap over child iterators. Above it, a filter layer walks in
internal-key order, keeps the first entry per user key with `seq <= snapshot`, skips the
rest, and emits nothing for a tombstone.

`Prev` is the expensive direction: the heap is rebuilt in the opposite order, and skipping
*backwards* over versions of one key requires looking ahead to find the newest visible one.
Implemented and tested, but documented as slower, and the randomized model test walks both
directions and compares (E-15).

---

## 4. Essential functions — the required surface

If it is not here at the end, the project is incomplete.

### 4.1 `include/lsmeng/` — public API

```cpp
// slice.h
class Slice { const char* data_; size_t size_; /* ... */ };  // no ownership, no NUL rules

// status.h
class Status {
 public:
  enum class Code { kOk, kNotFound, kCorruption, kIOError, kInvalidArgument, kNotSupported };
  static Status OK();
  static Status NotFound(Slice msg);        static Status Corruption(Slice msg);
  static Status IOError(Slice msg);         static Status InvalidArgument(Slice msg);
  static Status NotSupported(Slice msg);
  Code code() const;
  bool ok() const;
  bool IsNotFound() const;  bool IsCorruption() const;
  bool IsIOError() const;   bool IsInvalidArgument() const;
  std::string ToString() const;             // for humans only; format is NOT a contract
};

// env.h  -- every byte of I/O in the engine goes through this
class Env {
 public:
  static Env* Default();
  virtual ~Env();
  // sequential/positional files, append, Sync, Rename, CreateDir, FsyncDir,
  // DeleteFile, GetChildren, LockFile, NowMicros, ...
};

// options.h
struct Options {
  Env*   env                    = nullptr;  // nullptr => Env::Default(); tests pass a FaultEnv
  size_t write_buffer_size      = 4 << 20;  // memtable arena bytes before freeze
  size_t block_size             = 4 << 10;
  int    block_restart_interval = 16;
  int    bloom_bits_per_key     = 10;       // 0 disables filters entirely
  size_t block_cache_bytes      = 8 << 20;
  size_t filter_memory_bytes    = 16 << 20; // resident Bloom filters (S11, E-34)
  int    cache_shards           = 16;       // 1 = unsharded, for the T12 experiment
  int    tier_trigger           = 4;        // T
  int    max_tiers              = 7;        // §3.8.2
  int    max_open_files         = 500;
  bool   paranoid_checks        = true;
  bool   create_if_missing      = true;
  bool   error_if_exists        = false;
  bool   gc_orphans_on_open     = false;    // §3.9 -- deleting files is opt-in
};
struct ReadOptions  { const Snapshot* snapshot = nullptr; bool fill_cache = true;
                      bool verify_checksums = true; };
struct WriteOptions { bool sync = false; };

// write_batch.h
class WriteBatch { void Put(Slice,Slice); void Delete(Slice); void Clear();
                   size_t ApproximateSize() const; int Count() const; };

// iterator.h
class Iterator { bool Valid(); void SeekToFirst(); void SeekToLast(); void Seek(Slice);
                 void Next(); void Prev();
                 Slice key(); Slice value(); Status status(); };

// snapshot.h
class Snapshot;                              // opaque
class SnapshotHandle {                       // RAII; what tests and benchmarks must use
 public: explicit SnapshotHandle(DB*); ~SnapshotHandle(); const Snapshot* get() const;
};

// db.h
class DB {
 public:
  static Status Open(const Options&, const std::string& dir, DB** out);
  Status Put(const WriteOptions&, Slice key, Slice value);
  Status Delete(const WriteOptions&, Slice key);
  Status Write(const WriteOptions&, WriteBatch*);        // atomic
  Status Get(const ReadOptions&, Slice key, std::string* value);
  Iterator* NewIterator(const ReadOptions&);
  const Snapshot* GetSnapshot();  void ReleaseSnapshot(const Snapshot*);
  bool   GetProperty(Slice name, std::string* out);
  Status CompactRange(const Slice* begin, const Slice* end);  // nullptr = unbounded
  Status Flush();
  Status Close();     // idempotent; stops writes, joins the background thread, returns
                      // any sticky bg_error_ (S17)
  ~DB();              // calls Close() and discards the result -- an application that
                      // cares about the last error must call Close() itself
};
```

`nullptr` (not an empty `Slice`) means unbounded in `CompactRange`, because **the empty key
is legal** (§3.1) and cannot serve as a sentinel. `CompactRange` compacts, tier by tier from
tier 0 downward, every file whose range overlaps `[begin, end]`, merging each tier's
overlapping set into the next tier; with both bounds null it compacts everything to the
deepest tier, which is what the model test and the benchmarks need.

**`GetProperty` names** — the contract by which tests and benchmarks observe the engine:
`lsmeng.num-files-at-tier<N>` · `lsmeng.stats` · `lsmeng.memtable-bytes` ·
`lsmeng.bloom-checked` · `lsmeng.bloom-rejected` · `lsmeng.blocks-read` ·
`lsmeng.filter-blocks-read` · `lsmeng.filter-bytes-resident` · `lsmeng.cache-hits` ·
`lsmeng.cache-misses` · `lsmeng.wal-syncs` · `lsmeng.writes` · `lsmeng.compactions` ·
`lsmeng.bytes-compacted` · `lsmeng.pending-compaction-bytes` ·
`lsmeng.compaction-in-progress` (tier, or −1) · `lsmeng.stalls` · `lsmeng.max-stall-ms` ·
`lsmeng.orphan-files` · `lsmeng.orphan-bytes`.

### 4.2 Internal components (each independently testable)

`crc32c.h` (hardware `crc32` where available, table fallback) · `coding.h` (varint32/64,
fixed32/64, little-endian) · `hash.h` · `arena.h` · `skiplist.h` · `memtable.h` · `bloom.h` ·
`block_builder.h`/`block.h` · `sst_builder.h`/`sst_reader.h` · `table_cache.h` · `cache.h` ·
`wal_writer.h`/`wal_reader.h` · `version_edit.h`/`version_set.h` · `compaction.h` ·
`stats.h` · `histogram.h` · `env_posix.cc`.

**Every** file open, positional read, append, `fsync`, `rename`, directory `fsync` and
unlink performed anywhere below `DB::Open` goes through `options.env`. No engine translation
unit outside `env_posix.cc` calls libc I/O directly — that is what makes both the fault
injection reachable (§7 layer 2) and E-25's "there is no API to make this mistake"
enforceable (S24).

### 4.3 Tools and benchmarks

- `lsmeng_cli` — `put | get | del | scan | props | compact | bench | --gc`.
- `sst_dump` — dump an SST's footer, index, filter parameters, and entries. The debugging
  tool that makes E-8/E-9 tractable when a file reads back wrong.
- `bench_write`, `bench_read`, `bench_mixed`, `bench_cache` — §10.

---

## 5. Safety requirements — the checklist to re-read after every task

Walk this list top to bottom at the end of each task. Every item is either **still true**,
or the task is not finished. Items marked ⚙️ are enforced by a machine, not by reading.

| # | Requirement |
|---|---|
| S1 | An acknowledged `sync=true` write is in the WAL and `fsync`ed **before** the ack, and before its memtable insert is visible to any reader |
| S2 | A `sync=true` writer is never committed by a group whose leader does not `fsync` |
| S3 | Every public entry point validates key/value/batch size and rejects out-of-range input with `InvalidArgument` — never truncates, never asserts on user input |
| S4 | Every on-disk record (WAL, MANIFEST, SST block) carries a CRC32C verified before use; a `len == 0` record is invalid by construction; WAL/MANIFEST CRCs are seeded with the file's non-zero number |
| S5 | Durability ordering is never inverted: data file → fsync → manifest edit → fsync → in-memory install → delete of superseded files. **Log `N` is unlinked only after the flush of its memtable has a durable MANIFEST edit and `N < min_live_log`** (§3.3.1) |
| S6 | ⚙️ TSan clean under a workload with ≥4 writers, ≥4 readers, live flush and live compaction |
| S7 | No block, index entry, or filter is used without CRC verification when `paranoid_checks` is on |
| S8 | A missing or corrupt Bloom filter degrades to "maybe present" (read the file), never to "not present" |
| S9 | Sequence numbers are unique, monotonic, **allocated from 1**, and survive reopen (recovered from the manifest, not restarted from 0) |
| S10 | Exactly one comparator implementation exists, is a strict weak ordering, and is used by memtable, block builder, merging iterator, and compaction alike |
| S11 | No unbounded memory growth: memtable by `write_buffer_size`, block cache by `block_cache_bytes`, **resident Bloom filters by `filter_memory_bytes`** (the table cache evicts on whichever of that or `max_open_files` binds first), writer queue by a batch-size cap |
| S12 | ⚙️ Exactly one thread writes to a given memtable at a time (group-commit leader invariant), asserted in debug builds |
| S13 | ⚙️ `db_mutex_` is **never** held across a `read`/`write`/`fsync`/`rename`/`open`/`unlink`. Enforced by `TrackedMutex` + an `assert` at every blocking `Env` entry point — not by inspection. There are **no** exceptions |
| S14 | A file is unlinked only when no live `Version` references it; an in-flight reader's open fd keeps it readable regardless |
| S15 | A non-newest version is dropped only when the **previously emitted** version of the same user key is at or below `oldest_snapshot_seq`. A tombstone is additionally dropped only when `no_older_data_can_exist(user_key)` — no non-input file in the output tier **or any deeper tier** has a range containing the key — and its own sequence is at or below `oldest_snapshot_seq` |
| S16 | One process per database directory. Primary: `flock(LOCK_EX\|LOCK_NB)` on `LOCK`. **Fallback, always active:** `LOCK` created `O_CREAT\|O_EXCL` holding the owner's pid and `/proc/<pid>` start time; on `EEXIST`, `Open` returns `IOError("database is locked by pid N")` unless `/proc/N` is absent or its start time differs (stale → reclaim). `flock` semantics are **verified in this container** (T0), never assumed — `wanrep` B2 measured `flock` succeeding twice on a bind mount |
| S17 | Every error path returns a `Status`; the engine never `exit`s or `abort`s on a recoverable condition; an I/O error sets a sticky `bg_error_` that every subsequent `Write` returns |
| S18 | ⚙️ Every test using randomness prints its seed and honours `LSMENG_SEED` for exact replay |
| S19 | ⚙️ No leaks under valgrind memcheck at exit, including after an error-path `Open` failure and after `Close()` mid-compaction |
| S20 | ⚙️ ASan+UBSan clean and TSan clean on every suite — the three-configuration gate in `scripts/check.sh` |
| S21 | A read's sequence number **and** its memtable/imm/Version references are acquired in a **single** `db_mutex_` critical section. Capturing the sequence outside it is a stale-read bug |
| S22 | ⚙️ Every wait on `bg_cv_` has an escape predicate covering `bg_error_` and `shutting_down_`, and every write to either is followed by `notify_all()` under `db_mutex_`. A stalled writer must be woken by the *failure* of what it waits for, not only its success |
| S23 | ⚙️ MANIFEST appends are totally ordered and that order equals the order of in-memory `Version` installs. Exactly one thread appends; a second appender trips an assert |
| S24 | ⚙️ No engine code path performs I/O except through `Options::env`. `grep -nE '\b(open\|pread\|pwrite\|write\|fsync\|fdatasync\|rename\|unlink)\(' src/ \| grep -v env_posix.cc` returns nothing, and `check.sh` runs that grep as a test |
| S25 | No test branches on `Status::ToString()`; every test distinguishing outcomes uses `code()` or an `Is*()` predicate |

---

## 6. Edge cases anticipated in advance

The point is that when one of these shows up it is **recognised**, not discovered — and that
the silent ones get a test written *before* the code that might trip them.

Severity: 🔴 silent wrong answer or data loss · 🟠 crash/hang · 🟡 performance or accounting.

### Correctness of the read/merge path

**E-1 🔴 Recency ordering across and within tiers.** A key updated three times may have
versions in tiers 0, 1 and 2 at once. Files are searched tier-ascending, and within a tier
newest-first by file number. The trap: after a compaction the *output* file has a higher
number than files holding *newer* data in shallower tiers, so file number is a valid recency
key only **within** a tier — across tiers the guarantee is the §3.8.1 invariant.
*Defence:* the invariant and its proof; `test_recency.cpp` builds the exact interleaving
(write k=v1, flush, write k=v2, flush, compact tier 0 → tier 1, write k=v3, flush) and
asserts `Get(k) == v3`; plus the partial-tier case of E-30.

**E-2 🔴 Tombstone resurrection — five distinct ways.**
(a) the tombstone's key is not added to the Bloom filter, so its file is skipped and an
older value in a deeper tier is found;
(b) a tombstone is dropped by a compaction whose output is not the bottom tier while an
older value survives below;
(c) a tombstone is dropped that is newer than an active snapshot;
(d) **a version visible only through an old snapshot is dropped because the drop test used
the entry's own sequence rather than the previously emitted one** (the v1 bug, §11.3);
(e) **a tombstone is dropped at the bottom tier while a non-input, older sibling file
already in that same tier still holds the key** (the v1 `is_bottom_tier` bug, §11.2).
*Defence:* filter insertion covers all entry types, asserted in the builder; the two drop
rules of §3.8.3; `test_compaction_resurrection.cpp` with one case per letter — (d) is
`Put(k,v1); s=GetSnapshot(); Put(k,v2); Put(k,v3); Flush(); CompactRange(); Get(k, s)` must
still return `v1`; (e) drives k=v1 into the deepest tier by ordinary cascading compaction,
verifies placement via `num-files-at-tier<N>`, then deletes k and lets the tombstone reach
that tier by a compaction whose inputs exclude the file holding v1. The randomized model
test would catch all five by construction.

**E-3 🟠 Bloom filter with zero keys.** `n = 0` gives a zero-byte array; `% nbits` divides
by zero and the probe reads out of bounds. *Defence:* minimum 8-byte all-zero array, so
every probe hits a clear bit and answers "not present" — correct for an empty set.

**E-8 🔴 Prefix compression that reads back as subtly wrong keys.** An entry may only
reuse a prefix of the *immediately preceding* entry, and the first entry after a restart point
must set `shared = 0`. A builder that gets this wrong produces a file whose bytes are
internally consistent, so **every CRC passes** and the corruption is invisible until a key
comes back wrong.
*Defence:* T5's round-trip test compares a **full scan against the exact input vector**, not
spot lookups; the adversarial key sets (all-same, all-different, prefix chains, 0xFF, empty)
are what make a shared/non_shared miscount actually diverge; and `sst_dump` exists so the
divergence can be read off the file rather than guessed at.

**E-9 🔴 The index separator off by one.** The index key for block *i* must satisfy
`last_key(block_i) <= S < first_key(block_i+1)`. Get the boundary wrong by one and exactly the
keys sitting on a block boundary go missing — a handful of keys out of millions, with no error
anywhere.
*Defence:* a test that forces many small blocks (`block_size` set to a few hundred bytes) so
boundaries are dense, then scans every key; the randomized model test finds it by construction,
which is the reason layer 3 exists.

**E-15 🟡 Reverse iteration disagrees with forward.** *Defence:* the model test walks the
reference `std::map` both ways and compares full sequences.

**E-16 🔴 A key that is a prefix of another.** `"abc"` vs `"abcd"`: bytewise comparison with
shorter-is-smaller everywhere. `strcmp` would stop at a NUL; `memcmp` without the length
tiebreak would call them equal. *Defence:* one comparator (S10), brute-forced over a corpus
of prefixes, empty keys, embedded NULs and 0xFF bytes.

**E-17 🔴 User keys colliding with the encoding.** A user key ending in bytes that look like
a packed sequence number is fine *only* because the internal key appends a fixed 8 bytes and
the decoder splits by length. *Defence:* never parse an internal key by searching for a
delimiter; always `size - 8`, asserted in `ExtractUserKey`.

### Durability and recovery

**E-4 🔴 A `WriteBatch` half-applied after recovery.** *Defence:* one batch = one WAL
record, all-or-nothing because the CRC covers the whole payload. No fragmentation across
records, which is why the 8 MiB batch cap is a hard limit rather than a suggestion.

**E-5 🔴 Zero-filled WAL tail parsing as valid records.** `CRC32C("")` is `0`, so
`crc=0,len=0` is self-consistent and a sparse region reads as infinitely many valid empty
records. *Defence:* `len == 0` invalid by construction **and** the CRC seeded with the
non-zero `log_number`. Test: hand-craft a log with a 4 KiB zero region; recovery must stop
at the right offset.

**E-6 🟡→🔴 Orphan SSTs, and the danger of collecting them.** A crash between file creation
and the manifest edit leaves a file nothing references. Deleting it needs certainty that no
*other* process owns the directory — and S16's `flock` has already been measured failing on
this bind mount. *Defence:* orphans are **reported, not deleted**, by default
(`gc_orphans_on_open = false`); collection is opt-in and refused unless T7's two-process
exclusion test passed. §3.9.

**E-7 🔴 `rename` durable but the directory entry is not.** Without `fsync` on the
*directory*, a crash can lose the rename, leaving `CURRENT` naming a manifest that is gone —
an unopenable database. *Defence:* `fsync` the directory fd after every `rename` and every
file creation that must be visible; an `Env` fault mode drops un-fsynced directory entries
on simulated crash.

**E-18 🟠 `Open` on a non-database, or a half-created one.** Empty directory with
`create_if_missing=false`; `CURRENT` naming a missing manifest; a manifest whose first record
is not the comparator name; a directory owned by another live process. *Defence:* each is a
distinct `Status::Code` with a message naming the file, each with a test that constructs the
broken directory. A database that fails to open must say *why*.

**E-19 🔴 Sequence numbers restarting at 0 after reopen.** Post-restart writes would get
sequences *below* existing data, so old values shadow new ones forever. *Defence:*
`last_sequence` is a manifest field restored at open, then advanced past anything seen in
replay (S9). `test_reopen_seq.cpp`.

**E-20 🟠 Recovery of a log whose memtable overflows.** Replaying a 500 MiB log into a 4 MiB
memtable must not OOM. *Defence:* recovery flushes to an SST whenever the memtable exceeds
`write_buffer_size` and continues, exactly as the write path would.

**E-14 🟡 `fsync` here is not a power-loss guarantee.** Docker Desktop on macOS routes writes
through a VM and a shared-filesystem driver. `kill -9` tests process crash-consistency, which
the page cache survives; the un-fsynced case is covered separately by the `Env` crash
simulation. Stated in §3.3 and `BENCHMARKS.md` so it is never inflated.

**E-36 🔴 Two live logs at once.** From the moment a memtable is frozen until its flush has a
durable manifest edit, **two** logs hold acknowledged data. A recovery that replays only
`log_number` loses everything written since the switch. *Defence:* `prev_log_number` and
`min_live_log` (§3.3.1); per-file torn-tail handling; the crash test kills the writer
specifically during the window after a memtable switch.

### Concurrency

**E-21 🔴 The leader touching a retired memtable.** The leader captures the pointer under the
mutex, releases it, then inserts. *Defence:* switching happens only in `MakeRoomForWrite`, at
the queue front, under the mutex; debug assert in `MemTable::Add` (S12). The most delicate
thing in the design.

**E-22 🟠 Deadlock between a stalled writer and the background thread.** *Defence:* the
documented lock order; `wait` always releases `db_mutex_`; a watchdog that fails the stress
test after 60 s of no progress rather than hanging CI. Distinguished from E-31 by
`lsmeng.compaction-in-progress`.

**E-23 🟠 Background thread dying silently.** An I/O error kills the thread, the DB keeps
accepting writes until tier 0 grows without bound, and everything stalls forever with no
error surfaced. *Defence:* sticky `bg_error_` returned by every subsequent `Write` (S17), and
the stall path both checks it before waiting **and** re-checks it in its wait predicate, with
every setter broadcasting `bg_cv_` (S22).

**E-37 🟠 Lost wakeup on `bg_cv_`.** A writer already blocked in `MakeRoomForWrite` when
`bg_error_` is set, or when `Close()` begins, waits forever — and because it is the *leader*,
holding the leader slot, the whole database is out, not one thread. *Defence:* S22's escape
predicates and mandatory `notify_all()`; on early return the leader still runs step 7 (pop,
status, notify followers and the new front) or the queue keeps a corpse at its head and the
hang merely moves. Fault-injection case: inject `ENOSPC` into a compaction with 8 writers
stalled at `kTier0StopTrigger`; all 8 must return the sticky error.

**E-24 🟡 Statistics counters as accidental contention.** Per-op counters in one cache line
show up in a profile as false sharing and get *mistaken* for real contention. *Defence:*
per-thread counter blocks summed on read (or cache-line padding). Named here so that if T12's
profile points at them the diagnosis is fast.

**E-25 🔴 `read` instead of `pread` on a shared fd.** Two concurrent readers get interleaved
garbage that looks like an SST checksum bug. *Defence:* `Env` exposes only positional reads
(S24) — there is no API to make the mistake.

**E-38 🔴 Two threads appending to one MANIFEST.** Interleaved records fail CRC at the splice
point and §3.9's torn-tail rule then silently truncates every later edit. *Defence:* the
single-background-thread model (§3.8.4); a thread-id assertion on the manifest writer (S23);
T7's two-thread `LogAndApply` test exists to fail loudly if the model is ever broken.

**E-31 🟠 Head-of-line blocking between a deep compaction and the tier-0 stall.** The one
background thread is busy with a tier-3 merge; flushes fill tier 0 to the stop trigger; every
writer blocks and only the busy thread can unblock them. From outside this is
indistinguishable from the deadlock E-22 exists to detect. *Defence:* shallowest-eligible-
first bounds the damage to one deep merge; `lsmeng.compaction-in-progress`,
`pending-compaction-bytes` and `max-stall-ms` make it observable; the watchdog distinguishes
"progressing slowly" from "not progressing".

### Resource and scale

**E-11 🟡 Tier-0 backlog and the write stall.** Writes outrunning compaction is the normal
state of a loaded LSM. *Defence:* the slowdown/stop triggers. The spikes are *expected* and
are exactly what p99.9 is for — the tail is not noise, it is the design.

**E-10 🟡 A leaked snapshot freezes reclamation.** *Defence:* `SnapshotHandle` RAII, a debug
assert at `~DB`, and oldest-snapshot age in `lsmeng.stats`.

**E-12 🟡 Block-cache miss stampede.** Accepted by design (§3.10); listed so it is not
"discovered" later.

**E-13 🟡 Unbounded manifest growth.** *Defence:* snapshot-and-rotate at 64 MiB (§3.9).

**E-35 🔴 A `VersionEdit` appended to a manifest being rotated away.** *Defence:* rotation is
a mode of applying an edit, performed by the same single applier, snapshotting the version
*including* that edit; the old manifest is unlinked only after the `CURRENT` rename is
durable. Test: a fault-injection hook forces the 64 MiB trigger while a flush edit is in
flight; reopen and assert the flushed SST is in the recovered version and its log still
present.

**E-26 🟠 Open file descriptor exhaustion.** One fd per SST. *Defence:* the table cache is
bounded by `max_open_files` and evicts LRU readers. Tested with `max_open_files = 4` and 50
files.

**E-34 🟡 Resident Bloom filters as unbounded memory.** A whole-file filter grows with tier
depth — ~1.25 MiB for a deep-tier file — so the footprint is worst exactly where the file
count is smallest, and a count-based bound does not see it. *Defence:* `filter_memory_bytes`
bounds the table cache in bytes as well as count (S11); `lsmeng.filter-bytes-resident` makes
it visible.

**E-27 🟡 Space amplification during compaction.** Merging four 64 MiB files needs 256 MiB of
*extra* space before the inputs can be freed, so a 60%-full disk can fail a compaction.
*Defence:* documented as a property of the strategy; `ENOSPC` sets `bg_error_` and is
surfaced, and the partial output is deleted.

**E-32 🟡 Tier count grows with total bytes written, not live data.** Under an overwrite
workload every merge output is about the size of its inputs, so equally sized files
accumulate in different tiers where a count-per-tier rule can never pair them; read
amplification grows with them and the receding bottom tier suppresses tombstone reclamation.
*Defence:* `max_tiers` bounds it; `tier_sweep` measures files-consulted-per-`Get` and tier
count against total flushes on an overwrite workload; §3.8.2 states the limitation instead of
claiming STCS.

**E-33 🔴 `oldest_snapshot_seq` accidentally 0.** Compaction then drops nothing, reclaims no
space, and **every correctness test still passes** — a silent performance and space bug.
*Defence:* the empty snapshot list yields `last_sequence_`, not 0; sequences start at 1 so 0
is illegal and a debug assert rejects it; `test_compaction_reclaims.cpp` asserts live bytes
and file count strictly decrease and that a bottom-tier compaction with no live snapshot
leaves zero `kTypeDeletion` entries.

**E-28 🟡 A single enormous value distorting a block.** A 1 MiB value in a 4 KiB block means
one entry per block — legal, must not corrupt anything. *Defence:* the block builder flushes
on estimated size ≥ `block_size` **after adding at least one entry**, so an oversized value
produces a one-entry oversized block rather than an infinite loop or an empty block. Tested
at `block_size ± 1` and at the 1 MiB maximum.

**E-29 🟡 All keys identical, or distinct-but-adjacent.** Prefix compression's best and worst
cases; also thousands of versions of one key, which a `Get` must scan past. *Defence:* both
in the benchmark workload set; the many-versions case is a correctness test for the merging
iterator's dedup.

**E-30 🔴 A partial-tier compaction reordering recency.** Tier 0 holds f1..f6 (the compactor
is behind — the normal loaded state). If inputs were chosen by size or recency rather than
oldest-first, a newer version could be promoted to tier 1 while an older one stays in tier 0,
and `Get` would return the stale one forever. *Defence:* §3.8.1's invariant and the
oldest-prefix selection rule; a test that builds exactly this six-file tier-0 state and
asserts the newest value wins.

**E-39 🟡 Clock and timing assumptions.** Nothing may depend on wall-clock time for
correctness — not compaction scheduling, not snapshot ordering, not stall timing. Sequence
numbers are the only ordering authority. The single timing use is the 1 ms stall sleep, which
is a brake and cannot affect correctness.

---

## 7. Testing strategy

Five layers, weakest to strongest. The third finds the bugs the other four miss and is worth
more than all the hand-written cases combined.

**1. Unit tests, per component.** `crc32c` against the RFC 3720 vectors (so a hardware/table
mismatch is caught immediately) · varint round-trip including the 5- and 10-byte maximums ·
comparator strict-weak-ordering brute force · Bloom **measured** FPR vs theory · skip list vs
`std::set` · block builder → reader round-trip with adversarial key sets (all-same,
all-different, prefix chains, 0xFF, empty) · SST round-trip · WAL round-trip · histogram
percentile accuracy against a sorted array.

**2. Component fault injection.** `FaultEnv : public Env`, configured **per instance** (never
global, so tests run in parallel), able to inject at a chosen call index: short write, `EIO`,
`ENOSPC`, truncate at a byte offset, corrupt a byte, or drop un-fsynced writes on "crash".
Every durability claim in §5 is exercised through this seam. Building it into `Env` from T0
rather than retrofitting is deliberate: retrofitted fault injection tests the wrapper, not the
engine.

**3. Randomized differential testing against a model.** The one that matters.

```
   model  = std::map<std::string,std::string>
   engine = lsmeng::DB
   loop N times with a seeded RNG:
       pick an op: Put / Delete / Get / Scan(range) / batch / Flush /
                   CompactRange / Snapshot / ReleaseSnapshot / Reopen
       apply to both; compare results immediately
       every K ops: full forward scan AND full reverse scan of both, compared elementwise
```

Key space deliberately **small** (a few hundred keys) so overwrites, deletes and
resurrections collide constantly — a large key space hides exactly the merge bugs this hunts.
Seeds printed and replayable via `LSMENG_SEED` (S18). `Reopen` in the op mix makes it a
recovery test too. **Mismatch classification matters:** the harness compares
`Status::code()`, never `ToString()` (S25), because `NotFound` (model agrees → pass) and
`Corruption` (engine broken → must fail) are otherwise indistinguishable, which would let the
single most valuable test report a corrupted database as a successful miss.

**4. Crash tests.** Two configurations, because they test different things:

- **`kill -9` of a child process.** The child writes with `sync=true`, appending each
  acknowledged key to a separate journal that is itself fsynced *before* the ack is recorded,
  and is killed at a random point. The parent reopens and asserts: the DB opens; every
  acknowledged key is present with the right value; no partially-applied batch exists; no key
  never written appears. This tests framing, ordering and replay. It **cannot** test missing
  `fsync`, because the page cache survives a process death.
- **`FaultEnv` crash simulation**, which discards writes that were never `fsync`ed. This is
  the configuration in which a missing `fsync` actually fails a test. Both are required; the
  first alone would let S1 pass with every `fsync` deleted.

Kill points are swept across the interesting windows specifically — including the window
after a memtable switch when two logs are live (E-36) and during manifest rotation (E-35).

**5. Concurrency stress.** W writers + R readers + live flush + live compaction, readers
validating that every value seen is one actually written (values encode their key and a
sequence tag). Run under **TSan**, under **helgrind**, and with a watchdog that fails on 60 s
of no progress rather than hanging — and that reports `lsmeng.compaction-in-progress` so a
head-of-line stall (E-31) is distinguishable from a real deadlock (E-22).

**Two-process exclusion test** (its own small suite, because it needs a real second process):
a parent opens the DB and holds it; a fork/exec'd child calls `Open` on the same directory and
must return the distinct `Status` naming `LOCK`. Run on **both** the bind mount and the
container's own overlay filesystem, since the answer may differ. Both results are recorded as
`MEASURED` lines in `BENCHMARKS.md`, and the result gates `gc_orphans_on_open` (§3.9).

**The three-configuration gate** (`scripts/check.sh`, matching `dedupe`/`wanrep`): every suite
built and run under `none`, `address` (ASan+UBSan), and `thread` (TSan). ASan and TSan cannot
coexist in one binary, which is why it is three build trees. `check.sh` also runs the S24
grep as a test.

---

## 8. Build and environment

Ubuntu 24.04 + GCC 13 + CMake + Ninja in a container, bind-mounting the repo, exactly as
`dedupe` and `wanrep` do — same `scripts/dev.sh`, same `scripts/check.sh` shape. The container
is not tidiness: every correctness claim here is about `fsync` ordering, atomic `rename`,
directory durability, `flock` semantics and `pread` behaviour, and macOS/APFS and Linux/ext4
disagree about several of them.

Added over `dedupe`'s image: `linux-tools-generic` (perf), `python3` (plotting), `xxd`/`hexdump`
for reading our own formats.

**Database directories used by tests and benchmarks live on a container-local volume, not the
bind mount** — only source and results are bind-mounted. The bind mount is where `flock`
already failed once (`wanrep` B2) and where `fsync` latency is least representative.

**Environment facts measured in T0, never assumed** (`wanrep` §2.5 precedent), each becoming a
`MEASURED` line in `BENCHMARKS.md`:

- `fsync` latency on the bind mount **and** on the container-local volume;
- whether `flock` actually excludes on each (S16 branches on the answer);
- whether `perf` hardware counters are available (§10.4 predicts they are not);
- `nproc`, page size, and whether `_mm_crc32_u64` is usable;
- `docker info` must be the daemon check, not `docker version` — see `CHALLENGES.md` C2.

---

## 9. Task breakdown — the build order

Each task ends with: tests green in all three configurations, the §5 checklist walked, a
`CHALLENGES.md` entry for anything that went wrong, and a **stop** to explain what was done
before the next begins.

**On ordering, honestly.** The order is chosen so that every task is testable the same day it
is written. **Two forward dependencies remain, and are stated rather than hidden:**

- (i) The §7 layer-4 crash test needs a reader. So **T8 lands a minimal `Get`** over memtable
  / immutable memtable / tier 0 only — no merging iterator — enough to read a crash test back,
  and the full crash test is **re-run at T9** with the scan assertion once the merging
  iterator exists.
- (ii) **T9's model test runs the op mix minus `CompactRange`**, which T10 adds. T9 also caps
  the run so tier 0 cannot reach `kTier0StopTrigger` with no compactor to drain it — otherwise
  the model test would block forever on `bg_cv_`.

| Task | What | Ends when |
|---|---|---|
| **T0** | Container, CMake, three-config `check.sh`, test harness, `Env` + `FaultEnv`, `TrackedMutex`, environment facts | `check.sh` green on a trivial test; `FaultEnv` drives a DB-shaped directory through every injection mode; S13's assertion mechanism exists; `BENCHMARKS.md` has the T0 facts |
| **T1** | Primitives: `Slice`, `Status` (+ codes), `coding`, `crc32c` (hw + table), `hash`, `Arena`, the comparator | Unit tests incl. RFC 3720 vectors; comparator brute-forced over the adversarial corpus |
| **T1b** | `Stats`: per-thread counter block, `GetProperty` name parsing, all §4.1 property names, `lsmeng.stats` formatting | A counter incremented from 8 threads reads back exactly; TSan shows no false-sharing hotspot at the counter block (E-24) |
| **T2** | Bloom filter | **Measured** FPR within tolerance of theory across n and bits/key; E-3 covered; `bloom-checked`/`bloom-rejected` counters live |
| **T3** | Skip list + `MemTable` + iterator | vs `std::set`; TSan with 1 writer + 8 readers; arena-vs-user byte ratio **measured**; `memtable-bytes` live |
| **T4** | WAL writer/reader + single-file recovery | Round-trip; torn tail at **every** byte offset; E-5 zero-region; fault-injected short writes and `EIO` |
| **T5** | Block builder/reader, SST builder/reader, `sst_dump` | Round-trip vs the exact input vector; adversarial key sets; E-8/E-9/E-28; `blocks-read` live |
| **T6** | Sharded LRU block cache + table cache (byte- and count-bounded) | Correct under concurrent hammering (TSan); eviction bounded; `max_open_files` respected (E-26); `filter_memory_bytes` respected (E-34); cache counters live |
| **T7** | `VersionEdit`, `VersionSet`, MANIFEST, `CURRENT`, `Open`/recover, multi-log replay, `LOCK`, orphan **reporting** | Reopen preserves state; E-6/E-7/E-18/E-35/E-36/E-38 tests; directory-fsync test; two-thread `LogAndApply` test trips the S23 assert; **two-process exclusion test green on both filesystems** (gates `gc_orphans_on_open`) |
| **T8** | `DB` write path: group commit, memtable switch + log rotation, background thread, `Close()`, **plus a minimal `Get`** (memtable/imm/tier-0) | S1/S2/S12/S21/S22 tests; `test_reopen_seq.cpp` (E-19); crash tests pass using the minimal `Get`; `wal-syncs ≪ writes` at 8 threads; **S13 assertion active with no `Env` call observed under `db_mutex_` across the full suite**; a DB closed mid-compaction and mid-stall leaks nothing and does not hang |
| **T9** | Read path: full `Get`, merging iterator, snapshots + `SnapshotHandle`, `Scan`, `lsmeng_cli` (`put/get/del/scan/props`) | **Randomized model test green** on the op mix minus `CompactRange` — the milestone that matters most; layer-4 crash test re-run with the full-scan assertion |
| **T10** | Tiered compaction: oldest-prefix selection, both drop rules, `max_tiers`, stalls, `CompactRange` | Model test green on the **full** op mix; all five E-2 resurrection cases; E-30 partial-tier case; `test_compaction_reclaims.cpp` (E-33); no deadlock under stress; compaction/stall counters live |
| **T11** | Benchmarks + histogram + workloads + open-loop protocol; `BENCHMARKS.md` | R5/R6/R7/R10 numbers exist with their command lines and their baselines (§2.1) |
| **T12** | Profiling: `perf`, cachegrind/callgrind, helgrind, massif. Find one contention point, fix it, measure again | R11/R12/R13 have before/after numbers **and the mechanism**; `docs/CLAIMS.md` maps claims → evidence |

**Deliberately deferred:** benchmarking (T11) comes *after* correctness (T9, T10). Optimising
an engine that returns wrong answers is the most common way to waste a week, and a fast wrong
answer earns no partial credit.

---

## 10. Benchmarking and profiling plan

### 10.1 The measurement harness

A benchmark reporting only a mean is not evidence for R13, so the harness records **every**
operation's latency into a per-thread logarithmic-bucket histogram (~3 significant digits,
merged at the end) and reports p50/p90/p99/p99.9/max alongside throughput.

Every run records: git commit, thread count, key/value sizes, key distribution, sync mode,
options, machine, and the full command line. **A number without its command line does not go
in `BENCHMARKS.md`.**

**Coordinated omission, and the open-loop protocol.** A closed-loop benchmark (issue the next
op when the last returns) *cannot* observe a stall correctly: while the engine is stuck for
50 ms the loop simply issues fewer requests, and the histogram under-reports exactly the tail
R13 claims. So the harness also runs open-loop, measuring each operation from its *intended*
start time. But open loop is only meaningful **below saturation**, and §3.8.4's stop trigger
guarantees saturation is reachable — so the protocol is fixed rather than left to improvisation:

1. Every open-loop run is preceded by a closed-loop run on the same commit and workload to
   establish sustained throughput `M`. Targets are `{0.25, 0.50, 0.75, 0.90} × M`; both `M`
   and the fraction are recorded with the histogram.
2. **Validity criterion.** A run is INVALID and is not published if achieved throughput is
   below 99% of target, or if the issue backlog ever exceeds 100k operations. An invalid run
   is reported as "diverged at rate R", **never as a percentile** — otherwise the reported
   p99.9 is a function of run *duration*, not of the engine.
3. Backlog depth over time and the number of writers parked on `bg_cv_` are reported next to
   the percentiles, so a diverging run is visibly diverging.
4. **R12/R13 before/after comparisons are taken at a single fixed offered rate that *both*
   builds sustain** (the smaller `M` × 0.75) — never at each build's own saturation point.
5. Issuing model: a fixed pool of N threads draining a shared queue of `(intended_start, op)`
   bounded at 100k; a full queue is a divergence signal, not backpressure onto the schedule.

The headline write-path tail number is taken below the stall threshold; the stall-regime
number is reported separately and labelled "stalled regime (E-11)". Being able to explain this
distinction unprompted is worth more than the numbers — it is the most common way published
latency figures are wrong.

### 10.2 The workloads

| Name | Shape | Evidence for |
|---|---|---|
| `write_seq` | 1–16 threads, sequential keys, 16 B key / 100 B value, sync on and off | R5; group-commit scaling (B-selfscale); floor vs B-null |
| `write_rand` | as above, uniform random keys | R5 under realistic memtable/compaction load |
| `read_hot` | preloaded 10 M keys, Zipfian s=1.0, 1–16 readers | Cache and lock behaviour; R10 |
| `read_cold` | preloaded, uniform random, cache ≈10% of data | R6 — where filters actually pay |
| `read_absent` | lookups for keys never written | R6 directly: filters off → every candidate file read; on → ~0.8% |
| `mixed` | 2 writers + 8 readers, compaction live throughout | R8, R13 — the tail under compaction is the real number |
| `cache_shards` | `read_hot` with `cache_shards` ∈ {1, 4, 16} | R12 — the contention delta (B-selfconfig) |
| `bloom_sweep` | `read_absent` with `bloom_bits_per_key` ∈ {0, 4, 10, 16} | R6 — FPR vs memory, measured against theory |
| `tier_sweep` | `tier_trigger` ∈ {2, 4, 8}, **plus an overwrite variant over a 10k-key set run for ≥200 flushes** | R7 — write-amp/read-amp trade; and E-32: files-per-`Get` and tier count vs total flushes |

Each also records engine counters, because "p99 got worse" is a symptom and "blocks read per
`Get` went from 1.2 to 9" is a diagnosis.

### 10.3 What "before/after" means for R12 and R13

Not "it got faster" — a named change with a mechanism:

> *Under `read_hot` at 16 threads and a fixed offered rate both builds sustain, `perf record`
> attributed N% of cycles to `__lll_lock_wait` beneath the block cache's single mutex.
> Sharding 16 ways moved throughput from X to Y ops/s and p99.9 from A µs to B µs. The
> remaining contention moved to `db_mutex_` in `Get`'s capture step, which is the next thing
> to attack.*

The number, the mechanism, **and where the bottleneck moved** — because there is always a next
one, and naming it is what shows the profiler was read rather than run.

### 10.4 Tooling — including what will not work here, predicted in advance

**`perf`.** Under Docker Desktop on macOS the container runs in a LinuxKit VM with no exposed
PMU. Hardware events (`cycles`, `instructions`, `cache-misses`, `LLC-load-misses`) are expected
to report `<not supported>`, and `perf lock` needs kernel lock events that are not enabled.
**`MEASURED` in T0: the prediction was correct** — `cycles:u` and `instructions:u` both report
`<not supported>`, while the software `cpu-clock` event works. Every T12 profile uses it.

*If confirmed*, the substitutes are named now rather than improvised later:
- `perf record -e cpu-clock` — a **software** event, works under virtualisation, gives an
  accurate wall-clock sampling profile. Enough for R11's "profiled with perf" and for finding
  the lock-wait hotspot in R12.
- **cachegrind** for cache behaviour — it *simulates* a cache rather than reading counters, so
  it is immune to the missing PMU and is arguably the better tool here.
- **callgrind** for call-graph costs.
- Engine counters and the histogram for everything about contention a sampling profiler is bad
  at anyway.

If a Linux host with a real PMU is available, hardware numbers are taken there and labelled
with the machine. **What will not happen is a claim backed by a tool that printed
`<not supported>`.**

**Valgrind.** memcheck (leaks, S19) · helgrind and DRD (races and lock-order inversions, as a
second opinion to TSan — they disagree, and the disagreements are interesting) · massif
(memtable and cache footprint vs `write_buffer_size`, evidence for S11) · cachegrind and
callgrind as above. Valgrind is 20–50× slower, so it runs reduced workloads; a reduced workload
under helgrind still finds lock-order inversions, because those are structural, not statistical.

---

## 11. The review log — holes found by attacking version 1 on purpose

Version 1 was reviewed by six adversarial passes — durability, concurrency, LSM design,
scope/API, testability, and claim honesty — each instructed to **break the document, not
approve it**, and each finding was then handed to a skeptic instructed to **refute it** and to
default to "already covered" when uncertain.

**36 raised · 23 refuted · 21 survived and are fixed here.** Four were silent-data-loss bugs.

The refutation step matters as much as the attack. Of the 23 rejected, most were real-sounding
objections that §6 already answered — which is the argument for writing an edge-case inventory
*before* the code rather than after. The ones below are the ones that got through.

### The four that would have lost or corrupted data

**§11.1 — `is_bottom_tier` is not a sufficient tombstone-drop condition.** 🔴
*v1 said:* drop a tombstone when "the output tier is the last tier that exists", and called it
"cheap to check, impossible to get subtly wrong."
*The break:* tiered compaction writes its output **into a tier that already holds up to T−1
older, non-input files**, and the merge never reads them. Drive `k=v1` down to tier 2 by
cascading compaction; later `Delete(k)`; the tombstone's compaction outputs into tier 2, which
*is* the last tier, so the tombstone is dropped — and the older sibling file still in tier 2
resurrects `v1`. v1's own justification only ever reasoned about *deeper* tiers.
*Fixed by:* `no_older_data_can_exist(user_key)` (§3.8.3) — a range check against every
non-input file in the output tier **and** every deeper tier, using `FileMetaData` ranges, so no
I/O. S15 rewritten. E-2(e) and a test case added. LevelDB's `IsBaseLevelForKey` shortcut cannot
be copied because leveled compaction pulls the overlapping output-level files into its inputs
and tiered does not — a sentence worth being able to say out loud.

**§11.2 — the dedup rule dropped exactly the version a snapshot needs.** 🔴
*v1 said:* `drop = (entry.seq <= oldest_snapshot_seq)`.
*The break:* the predicate must be about the **previously emitted** version, not this one.
`Put(k,v1)@40; snapshot@50; Put(k,v2)@60; Put(k,v3)@100`. The merge emits v3 (newest), emits v2
(60 > 50), then evaluates v1: `40 <= 50` → **dropped**. Reading through the snapshot now finds
nothing at or below 50, so `Get` returns `NotFound` for a key the snapshot must see as `v1`.
Worse than a one-off: the first version at or below the boundary *always* satisfies the test,
so the rule dropped the needed version every time.
*Fixed by:* the `last_seq_for_key` formulation in §3.8.3, with the justification stated —
*an entry is unobservable only if the version that supersedes it is itself visible to the
oldest live snapshot.* S15 rewritten; E-2(d) and a test case added.

**§11.3 — only one WAL was live, but two always are.** 🔴
*v1 said:* one `log_number`, one replay rule, "a log may be deleted after its memtable is
flushed."
*The break:* from the moment `MakeRoomForWrite` freezes a memtable until that flush has a
durable manifest edit, **two logs hold acknowledged data** — the one feeding the immutable
memtable and the one feeding the mutable one. A crash in that window, with recovery replaying
only `log_number`, loses every acknowledged `sync=true` write since the switch. v1 had no
`prev_log_number`, no multi-file replay, and no liveness predicate for GC.
*Fixed by:* §3.3.1 — `prev_log_number`, `min_live_log`, ascending multi-file replay, a
**per-file** torn-tail rule (a truncated earlier log followed by an intact later one is
*normal*, not a stopping condition), and a GC rule that keeps every log ≥ `min_live_log`. S5
rewritten; E-36 added; the crash test now sweeps kill points through that window specifically.

**§11.4 — compaction input selection broke the invariant the read path depends on.** 🔴
*v1 said:* trigger on "T **similarly-sized** files in a tier", and separately "tier `t` is
compacted when it holds T = 4 files. All four are merged" — undefined once a tier holds more
than four, which tier 0 provably does (the stall triggers are 8 and 12).
*The break:* §3.7's correctness rests on an invariant v1 never stated — *for any user key, the
shallowest tier containing it holds its newest version* — which holds only if a compaction
consumes a **recency-contiguous oldest prefix**. Size-based selection contradicts it: tier 0
holds f1..f6, f1 has `k=v1` and f5 has `k=v2`; a size-chosen subset {f3,f4,f5,f6} promotes
`v2` to tier 1 while `v1` stays in tier 0, which is searched first — `Get(k)` returns the stale
`v1` forever, with no crash, no checksum failure and no counter showing anything wrong.
*Fixed by:* §3.8.1 states the invariant **with its proof**, and §3.8.2 fixes selection as "the
T lowest-numbered files, always". "Similarly-sized" is removed from the trigger and demoted to
a *consequence*. R7 reworded. E-30 added with the six-file test.

### Concurrency and durability

**§11.5 — the read path captured its sequence number outside the lock.** 🔴
v1's step 1 read `last_sequence_`, step 2 then took `db_mutex_` to pin the Version. In the gap
a compaction can install a Version that legitimately dropped the version this read needs; the
reader pins the *new* one and misses. Silent stale read. *Fixed:* both in one critical section
(§3.7), with the superset argument written out, plus S21 and the same rule for `NewIterator`.

**§11.6 — two background threads, one MANIFEST, no lock.** 🔴
v1 described a flush thread and a compaction thread, both appending `VersionEdit`s, while S13
forbade holding `db_mutex_` across `fsync` — so the appends provably ran with no lock, and
§3.10's "exhaustive" inventory had nothing for them. Two unsynchronised appenders on one fd
interleave bytes mid-record; the CRC fails at the splice; §3.9's torn-tail rule then silently
truncates **both edits and every later one**. *Fixed:* §3.8.4 collapses to **one background
thread**, which removes the race rather than guarding it, and makes append order equal install
order by construction (S23, E-38). The alternative — two threads plus a manifest mutex
knowingly held across I/O — was considered and rejected; §3.10 records why, and §3.8.4 records
the price (head-of-line blocking, E-31, made observable rather than hidden).

**§11.7 — manifest rotation could discard a durable edit with no crash involved.** 🔴
v1 specified rotation in one sentence, naming no thread and no exclusion. A flush edit appended
to the *outgoing* manifest, while rotation snapshots the *pre-edit* version, is lost — and S5
then permits deleting the WAL that held that data, because the edit was durable. Every
individual rule obeyed; data gone. *Fixed:* rotation is a **mode of applying an edit** by the
same single applier, snapshotting the version *including* that edit; no edit is appended to a
manifest after `CURRENT` stops naming it; the old manifest is unlinked only after the
`fsync(dir)` (E-35).

**§11.8 — a lost wakeup on `bg_cv_` is a whole-database outage.** 🟠
A writer already blocked in `MakeRoomForWrite` when `bg_error_` is set waits forever — and
because it is the *leader*, holding the leader slot, nothing else can proceed either. v1's
E-23 defence only checked `bg_error_` *before* waiting. *Fixed:* S22 — escape predicates
covering `bg_error_` and `shutting_down_`, mandatory `notify_all()` under `db_mutex_` from
every setter, and the rule that a leader taking an early return still runs step 7 or the queue
keeps a corpse at its head. E-37 added, with an `ENOSPC`-under-8-stalled-writers test.

**§11.9 — `oldest_snapshot_seq` had no defined value for an empty snapshot list.** 🔴
An accidental `0` makes compaction drop nothing, reclaim no space, and **pass every
correctness test**. *Fixed:* §3.8.3 — empty list yields `last_sequence_`, captured once under
the mutex at schedule time and held constant for the merge (with the reason a mid-merge re-read
is wrong); sequences start at 1 so `0` is illegal and asserted. E-33 and
`test_compaction_reclaims.cpp` added — no test in v1 would have failed on this.

**§11.10 — S13 was violated by v1's own write path, and nothing could detect it.** 🟠
A memtable switch creates a log file, which is `open` + directory `fsync` — under `db_mutex_`,
inside `MakeRoomForWrite`, blocking every queued writer and every `Get` behind a
several-millisecond sync, directly inside the metric R13 claims to improve. And holding a lock
across I/O is neither a data race nor a lock-order inversion, so TSan, helgrind and a
lock-order tracker all report nothing; v1's only enforcement was the word "inspection".
*Fixed:* §3.2.1 — logs are pre-created by the background thread, and the fallback path releases
the mutex before creating one; plus `TrackedMutex` and an `assert` at every blocking `Env`
entry point, so S13 is enforced by a machine (T0 deliverable, T8 exit criterion).

### Scope, API, and evidence

**§11.11 — the fault-injection strategy could not reach the DB.** `Options` had no `Env*` and
`DB::Open` took no `Env`, so §7's entire layer-2 and layer-4 apparatus was unreachable. *Fixed:*
`Options::env`, plus S24 and a grep run as a test — which also makes E-25's "there is no API to
make this mistake" enforceable rather than aspirational.

**§11.12 — T7 and T8 could not meet their own exit criteria.** T7 required E-19's
write-reopen-overwrite test (needs `Put` from T8 and `Get` from T9); T8 required crash tests
(needs `Get` and a full scan from T9). §9's claim that "nothing depends on something not yet
built" was false at the two largest tasks. *Fixed:* T8 gains a **minimal `Get`** over
memtable/imm/tier-0, the full crash test re-runs at T9, and §9 now **states the two remaining
forward dependencies instead of claiming there are none**.

**§11.13 — deliverables §4 called mandatory were built by no task.** `lsmeng_cli`, the
`GetProperty` counters, and the `LOCK` file appeared in the API and in test descriptions but in
no task row. *Fixed:* **T1b** added for the stats/property layer; every later task now names the
counters it owns; the CLI is placed in T9/T10/T11; the `LOCK` is named explicitly in T7.

**§11.14 — the API could not express the distinctions the tests need.** `Status` had only
`ok()`/`ToString()`, so the model test could not tell `NotFound` (model agrees → pass) from
`Corruption` (engine broken → must fail) without string-matching a format the spec never
defined — the single most valuable test would have reported a corrupted database as a
successful miss. `Snapshot` was never declared, yet §3.12 called the handle "RAII" while the
API was a manual get/release pair. `Close()` was required by R1 and absent. *Fixed:* `Status`
codes and `Is*()` predicates plus S25; `SnapshotHandle` as the RAII wrapper with the raw pair
kept for C-style embedders; `Status Close()`; and `CompactRange(const Slice*, const Slice*)`,
because **the empty key is legal** and cannot be a sentinel.

**§11.15 — the whole-file Bloom filter broke two claims at once.** "No I/O at all" is false if
filters are fetched through the block cache (F filter reads before any file is skipped), and a
deep-tier filter is ~1.25 MiB — 15% of the default cache, evicting the data blocks it exists to
protect. If instead pinned at table-open, filter memory is `max_open_files × filter_size`,
entirely outside S11, which bounded open files by *count*. *Fixed:* filters are pinned in the
`SstReader` (§3.5), the range check moves ahead of the filter check in §3.7, the claim narrows
to "no **data block** read", `filter_memory_bytes` is added to `Options` and S11, and
`lsmeng.filter-blocks-read` is reported next to `blocks-read` so the filter's own I/O is
visible rather than hidden inside the win. E-34.

**§11.16 — one background thread turns the tier-0 stall into a multi-minute outage.** A
tier-3 merge is hundreds of MiB; while it runs nothing can drain tier 0, so writers hit the stop
trigger and block for the merge's duration — and from outside this is indistinguishable from the
deadlock E-22 exists to detect. *Fixed:* shallowest-eligible-first is now stated policy, the
head-of-line cost is documented rather than discovered, `compaction-in-progress`,
`pending-compaction-bytes` and `max-stall-ms` make it observable, the watchdog distinguishes the
two, and the two-worker upgrade is named as the next step. E-31.

**§11.17 — "tiered" was count-triggered, and tier count grows with total writes.** Under an
overwrite workload every merge output is about the size of its inputs, so equally sized files
accumulate in different tiers where a count-per-tier rule can never pair them — R7's own success
criterion ("read amplification bounded") fails, and the receding bottom tier suppresses tombstone
reclamation. *Fixed:* `max_tiers` bounds it (deepest-tier compactions consume the whole tier, so
§3.8.1 still holds); §3.8.2 **states plainly that this is not Cassandra-style STCS and why**;
`tier_sweep` gains an overwrite variant that measures files-per-`Get` and tier count against
total flushes. E-32. Size-bucketed selection is named as the next step, with the reason it is not
in v1: it needs its own proof that it preserves the ordering invariant.

**§11.18 — the S16 lock rests on a fact `wanrep` already measured as false.** `flock` succeeded
twice on this exact bind mount in `wanrep` B2. v1 recorded that candidly, scheduled a T0
re-measurement — and then specified nothing for the likely failure, while wiring an
**unconditional `unlink` loop** to it. *Fixed:* S16 gains an always-active pid-file fallback with
a stale-lock check; §7 gains a real two-process exclusion test run on both filesystems; and, most
importantly, **orphan collection is decoupled from the lock entirely** — `gc_orphans_on_open`
defaults to `false`, orphans are *reported*, and collection is refused unless the exclusion test
passed. An orphan wastes space; a wrongly unlinked live SST ends the database.

**§11.19 — open-loop mode was one sentence and not implementable.** With a fixed schedule and a
service rate below the offered rate, backlog grows without bound and the reported p99.9 becomes a
function of run *duration* — and §3.8.4's stop trigger makes that the *designed* behaviour, not a
corner case. Two builds could not be compared at all, which is exactly what R13 needs. *Fixed:*
§10.1's five-point protocol — calibrate `M` closed-loop first, target fractions of it, a validity
criterion that reports "diverged at rate R" instead of a percentile, backlog reported alongside,
and **before/after comparisons at a single fixed rate both builds sustain**.

**§11.20 — "high throughput" and "fast reads" had no comparand.** *Fixed:* §2.1 names three
baselines — B-null (raw `write`+`fsync`, the floor), B-selfscale (1 vs N threads, which is what
group commit actually claims), and B-selfconfig (same code, one variable: filters off, one cache
shard, sync on/off) — and states explicitly that we do **not** benchmark against RocksDB and
claim victory, because a 2 kLOC engine beating a mature one would mean the workload was rigged.

**§11.21 — the crash test could not fail when `fsync` was missing.** `kill -9` leaves the page
cache intact, so a build with every `fsync` deleted would still pass. v1 mentioned the `Env`
simulation in the same paragraph, blurring two different experiments into one. *Fixed:* §7 layer
4 is now explicitly **two configurations**, with a sentence naming what each can and cannot
falsify.

### What the skeptics threw out

Recorded because the refutations are as instructive as the findings. Rejected objections
included: "the WAL needs LevelDB-style block framing" (the `len==0` ban plus the seeded CRC
already close the resync hole E-5 describes); "the skip list needs hazard pointers" (nothing is
ever deleted from a memtable — §3.4 says so and says why); "concurrent `pread` on a shared fd is
a race" (it is not; `read` would be, which is why `Env` exposes only positional reads); and
several restatements of trade-offs §6 already accepts and documents (cache-miss stampede,
space amplification, write stalls). The pattern is consistent: **an objection that the document
already answers costs nothing to dismiss, and an objection it does not answer is usually a bug.**
That is the argument for writing §5 and §6 before the code.
