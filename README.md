# lsmeng

A persistent, ordered key–value storage engine written from scratch in C++20 — the layer
that sits underneath a database and actually puts bytes on disk.

It implements its own write-ahead log, skip-list memtable, SST file format, Bloom filter,
block index, sharded LRU block cache, tiered compaction and crash-consistent manifest.
There are no third-party libraries: the only things linked are `libstdc++` and `pthread`,
and the test harness is part of the repository.

```
~6,700 lines of engine  ·  ~5,700 lines of tests  ·  22 test binaries
green under -O2, ASan+UBSan and TSan
```

## Contents

- [Design in one paragraph](#design-in-one-paragraph)
- [Quick start](#quick-start)
- [Using the library](#using-the-library)
- [Architecture](#architecture)
- [Design decisions](#design-decisions)
- [Measured results](#measured-results)
- [Testing](#testing)
- [Configuration](#configuration)
- [Tools](#tools)
- [Repository layout](#repository-layout)
- [Known limitations](#known-limitations)
- [Documentation](#documentation)
- [License](#license)

## Design in one paragraph

Disks are fast when writing sequentially and slow when seeking, so this engine never
modifies a file that already exists. The sorted structure lives in memory and disk only
ever receives appends. That single decision creates three problems, and each is a
subsystem: memory is lost in a crash, so every write is first appended to a **write-ahead
log**; immutable files accumulate forever, so a background thread runs **compaction**; and
the engine's knowledge of its own files is also in memory, so it is recorded in an
append-only **manifest**. This is the log-structured merge tree, and the trade it makes is
cheap writes paid for with more expensive reads, bought back with Bloom filters and
compaction.

## Quick start

Everything builds and runs inside a container. That is deliberate: the correctness
properties here are about `fsync` ordering, atomic `rename`, directory durability, `flock`
semantics and positional reads, and macOS and Linux disagree on several of them.

**Prerequisite:** Docker (Desktop is fine). Nothing else to install — the image carries
GCC 13, CMake, Ninja, GDB, Valgrind and perf.

```bash
git clone https://github.com/alladikarthik02/High-Performance-Concurrent-Storage-Engine.git
cd High-Performance-Concurrent-Storage-Engine

# Build and run every suite in all three configurations. First run builds the image.
./scripts/dev.sh ./scripts/check.sh
```

`check.sh` configures and builds three separate trees and runs the full suite in each:

| Configuration | Directory | What it catches |
|---|---|---|
| `none` | `build-none` | the `-O2` build that is benchmarked |
| `address` | `build-address` | ASan + UBSan: memory safety, undefined behaviour |
| `thread` | `build-thread` | TSan: data races, lock-order inversions |

ASan and TSan cannot be combined in one binary, which is why there are three trees.

Running pieces individually:

```bash
./scripts/dev.sh ./build-none/test_wal          # one suite
./scripts/dev.sh ./build-none/bench --bench fillrandom --num 10000 --db /data/x
./scripts/dev.sh                                # interactive shell in the container
```

## Using the library

```cpp
#include "lsmeng/db.h"

using namespace lsmeng;

Options options;
options.create_if_missing = true;

DB* db = nullptr;
Status s = DB::Open(options, "/data/mydb", &db);
if (!s.ok()) { /* s.ToString() */ }

// Write. sync=false returns once the record is in the log and the memtable;
// sync=true additionally fsyncs before returning.
WriteOptions wo;
wo.sync = true;
s = db->Put(wo, "user:42", "karthik");

// Read.
std::string value;
s = db->Get(ReadOptions(), "user:42", &value);
if (s.IsNotFound()) { /* absent, or deleted */ }

// Atomic batch: all of it is applied, or none of it.
WriteBatch batch;
batch.Put("a", "1");
batch.Delete("b");
s = db->Write(WriteOptions(), &batch);

// Ordered scan.
Iterator* it = db->NewIterator(ReadOptions());
for (it->Seek("user:"); it->Valid(); it->Next()) {
  // it->key(), it->value()
}
delete it;

// A snapshot pins a consistent view while writes continue.
{
  SnapshotHandle snap(db);
  ReadOptions ro;
  ro.snapshot = snap.get();
  s = db->Get(ro, "user:42", &value);
}

s = db->Close();
delete db;
```

Every operation that can fail returns a `Status` carrying a machine-readable code. Nothing
throws, and a missing key is an ordinary outcome rather than an exception.

## Architecture

```
                       Put(k,v)        Get(k)
                           │              │
  ═════════════════════════│══════════════│══════════════════════
        IN MEMORY          │              │     (lost on a crash)
                           ▼              │
                    ┌──────────────┐      │
                    │   memtable   │◄─────┤   live, taking writes
                    │  (skip list) │      │
                    └──────────────┘      │
                           │ fills up     │
                           ▼              │
                    ┌──────────────┐      │
                    │     imm      │◄─────┤   frozen, being flushed
                    └──────────────┘      │
  ═════════════════════════│══════════════│══════════════════════
        ON DISK            │ flush        │     (survives a crash)
   ┌─────────┐             ▼              │
   │   WAL   │     tier 0: [sst][sst][sst][sst]  ◄──┤  newest
   └─────────┘                   │ compaction       │
                 tier 1: [  sst  ][  sst  ]       ◄──┤
                                 │                   │
                 tier 2: [      sst      ]        ◄──┘  oldest

   CURRENT ──► MANIFEST     which SSTs exist, their tier and key range
```

**Write path.** Writers join a queue under one mutex. The thread at the front becomes the
leader, merges every waiting writer's batch into one, then **releases the mutex** and does
the log append, the `fsync` and the memtable inserts with no lock held, before re-taking it
to publish sequence numbers and wake the followers. One `fsync` commits many writers.

**Read path.** Newest to oldest — memtable, frozen memtable, then tiers — stopping at the
first match, so the newest version wins by search order. Within each SST a lookup passes
through four filters that live in memory before touching the disk once:

```
file key range (manifest) → Bloom filter → block index → read ONE block
        free                 nanoseconds      in memory      the only I/O
```

**Background.** A single thread flushes frozen memtables into tier 0 and compacts the
oldest files of a full tier down a level, dropping superseded versions and expired
tombstones. If writers outrun it, they are slowed and then blocked rather than allowed to
degrade the database.

**Recovery.** `CURRENT` names the live manifest; replaying the manifest rebuilds the file
set, and replaying the remaining logs rebuilds the memtable.

## Design decisions

**A skip list, not a balanced tree.** Insertion only allocates a node and publishes forward
pointers, so a reader can never observe a transiently inconsistent structure. A red-black
tree rotates, and a rotation is exactly the intermediate state that would force every read
to take a lock. One writer, any number of concurrent readers, no lock on the read path. The
memory ordering is a release store on publication paired with acquire loads on traversal.

**Group commit.** `fsync` costs the same whether it commits one write or twenty, so writers
queue and one commits the group. The honest counterpart: with `sync=false` there is nothing
to amortise, and throughput falls with thread count because the serialisation remains.

**No lock is ever held across file I/O.** That rule is neither a data race nor a lock-order
inversion, so no sanitizer can detect a violation and no correctness test can fail on one.
It is enforced at runtime instead: the mutex sets a bit in a thread-local mask and every
blocking `Env` entry point asserts the bit is clear.

**Immutable, refcounted versions.** A read takes the lock once to capture its sequence
number and reference the memtable, frozen memtable and current file set together, then does
all its work unlocked. Compaction installs a *new* version; the old one survives until the
last reader releases it.

**Tombstones and the compaction drop rules.** A delete writes a marker. An older version may
be dropped only when the version that supersedes it is visible to the oldest live snapshot,
and a tombstone only when no file outside the merge, in the output tier or deeper, could
hold the key. Both rules exist because the obvious versions silently resurrect deleted data.

**Checksums everywhere.** Every WAL record, manifest record and SST block carries a CRC32C
verified before use, with three implementations selected at runtime (ARM, x86-64, portable).
A zero-length record is invalid by construction and log checksums are seeded with the file
number, because the CRC of an empty message is zero and a region of zeros would otherwise
validate as a well-formed record.

## Measured results

Every number below is reproducible; `docs/BENCHMARKS.md` carries the exact command line for
each one.

| | Result |
|---|---|
| Group commit, 1 → 16 threads, `sync=true` | **6.9× throughput while the fsync count falls 8.5×** (8.48 writes per fsync at 16 threads) |
| Single-threaded durable writes | **99.6% of a raw append-plus-fsync floor**, with a full sorted index maintained |
| Bloom filters, absent-key lookups | **38× fewer data blocks read** (261,128 → 6,819), 2.4× throughput; measured FPR 0.836% vs 0.82% theory |
| Block-cache sharding, 1 → 16 shards | 1.58× throughput, p90 29 → 10 µs |
| Removing two allocations per lookup | +41% throughput, and run-to-run spread 24% → 1.4% |
| Differential test vs `std::map` | 30,000 random operations, 299 compactions, 592 reopens, **zero mismatches** |
| helgrind, DRD, massif | 0 errors, 0 errors, peak heap 11.55 MiB against a 12 MiB budget |

Reproduce the headline write benchmark:

```bash
./scripts/dev.sh ./build-none/bench --bench fillrandom --threads 16 --num 40000 \
                                    --sync true --db /data/ws
```

## Testing

- **Three sanitizer configurations**, all green, gated by `scripts/check.sh`.
- **Differential testing** against `std::map` with a randomised operation mix — puts,
  deletes, batches, flushes, compactions, reopens and scans — comparing after every
  operation. This is the primary net, because the dangerous bugs here produce a wrong
  answer rather than a crash.
- **Fault injection** through a pluggable `Env` that drops un-`fsync`ed bytes, un-durable
  file creations and un-durable renames, which is how durability is asserted rather than
  assumed.
- **Exhaustive recovery tests**: the log is truncated at every byte offset and a flipped bit
  is walked through every byte position, asserting that recovery always returns a clean
  prefix and never a record with wrong contents.
- Every randomised test prints its seed and honours `LSMENG_SEED` for exact replay.

## Configuration

Defaults, all in `include/lsmeng/options.h`:

| Option | Default | Meaning |
|---|---|---|
| `write_buffer_size` | 4 MiB | memtable size before it is frozen |
| `block_size` | 4 KiB | SST data block target |
| `block_restart_interval` | 16 | full key stored every N entries |
| `bloom_bits_per_key` | 10 | 0 disables filters |
| `block_cache_bytes` | 8 MiB | block cache budget |
| `filter_memory_bytes` | 16 MiB | resident Bloom filter budget |
| `cache_shards` | 16 | cache shard count |
| `tier_trigger` | 4 | files in a tier before it compacts |
| `max_tiers` | 7 | deepest tier merges in place beyond this |
| `tier0_slowdown_trigger` | 8 | writes take a 1 ms brake past this |
| `tier0_stop_trigger` | 12 | writes block past this |
| `paranoid_checks` | true | verify checksums on every block read |

## Tools

```bash
./build-none/sst_dump <file.sst> [--entries]   # footer, index, filter and block contents
./build-none/env_facts                          # measured filesystem and CPU facts
./build-none/bench --bench <name> ...           # fillseq, fillrandom, readrandom,
                                                # readhot, readmissing, mixed, baseline
```

## Repository layout

```
include/lsmeng/   public and internal headers; design rationale lives in these comments
src/              engine implementation
tests/            22 suites plus the test harness and the fault-injecting Env
bench/            benchmark harness with latency histograms and an open-loop mode
tools/            sst_dump, env_facts
scripts/          dev.sh (run anything in the container), check.sh (the full gate)
docs/             specification, bug journal, benchmarks, claims, safety audit
```

## Known limitations

Stated rather than hidden; most are deliberate trades.

- **Read amplification** is inherent to the design. Filters and indexes reduce it; a B-tree
  would beat it on read-heavy workloads.
- **Write amplification**: every byte is written at least twice, once to the log and again
  into an SST, plus once per compaction it passes through.
- **One background thread** does both flushes and compactions and is never preempted, so a
  deep merge can delay an urgent tier-0 flush. This was chosen so that only one thread ever
  writes the manifest, which removes a class of race rather than guarding against it.
- **Durability is verified against process crashes**, using `kill -9` and an `Env` that
  drops un-`fsync`ed writes. Power-loss durability is not claimed: `fsync` inside a
  container on Docker Desktop for macOS crosses a virtualisation boundary.
- **One process per database directory**, enforced with `flock` plus a pid-based fallback.
- **No block compression.** The format reserves a type byte for it; only uncompressed
  blocks are written and anything else is rejected. Keys are prefix-compressed within a
  block, which is a separate mechanism.
- Compaction is count-triggered tiering, not size-bucketed; `max_tiers` bounds the growth
  this can cause on overwrite-heavy workloads.

## Documentation

| Document | Contents |
|---|---|
| [`docs/SPEC.md`](docs/SPEC.md) | the design: architecture, API, 25 safety requirements, 39 anticipated edge cases, the build order, and a review log of 21 defects found by attacking the first draft on purpose |
| [`docs/CHALLENGES.md`](docs/CHALLENGES.md) | a bug journal — every problem hit while building, with the wrong hypotheses kept, not just the fix |
| [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) | every measurement with the command line that produced it |
| [`docs/CLAIMS.md`](docs/CLAIMS.md) | each claim about this engine mapped to the measurement that earns it |
| [`docs/SAFETY-AUDIT.md`](docs/SAFETY-AUDIT.md) | all 25 safety requirements with their evidence |

## License

BSD 3-Clause. See [`LICENSE`](LICENSE).
