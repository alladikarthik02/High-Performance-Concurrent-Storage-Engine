# lsmeng

A key–value storage engine written from scratch in C++20.

A storage engine is the part of a database that nobody sees: the layer underneath the query
parser and the network protocol that actually decides how bytes get onto a disk and how
they are found again. Postgres has one, SQLite has one, and RocksDB is one on its own. This
is that layer, built from nothing — its own write-ahead log, memtable, file format, Bloom
filter, cache, compaction and crash recovery.

There are no third-party libraries. The only things linked are `libstdc++` and `pthread`,
and even the test harness is part of the repository. That is roughly 6,700 lines of engine
and 5,700 lines of tests, in 22 test binaries that run green under `-O2`, under
AddressSanitizer with UndefinedBehaviorSanitizer, and under ThreadSanitizer.

## Why it is built this way

Start with the awkward fact that shapes everything else: a disk is fast when you write in a
straight line and slow when you jump around. Writing a megabyte in one continuous stretch
is cheap. Writing that same megabyte as a thousand small updates scattered across the disk
is dramatically more expensive.

A database also needs its data sorted, otherwise range scans are impossible and lookups are
hopeless. But keeping a file sorted means inserting into the middle of it, and inserting
into the middle of a 10 GB file means rewriting most of it. That is exactly the expensive
pattern.

The usual answer is a B-tree, which keeps data sorted on disk in fixed-size pages and
rewrites one page per insert. It is a good answer, and it makes reads very fast, because a
lookup walks a short path to exactly one place. But a random key means a random page, so
writes are still scattered.

This engine takes the other side of that trade. The rule is:

> **Keep the sorted structure in memory. Only ever append to disk. Never modify a file that
> already exists.**

Writes become appends, which is the cheap pattern. An update just appends a newer record
that wins over the old one. A delete appends a marker, called a tombstone, which hides the
old value rather than erasing it.

That one decision is nice for writes and creates three new problems, and most of this
codebase is those three problems being solved:

1. **Memory is lost in a crash.** The sorted structure lives in RAM, so before any write
   becomes visible it is appended to a log on disk. After a crash, the log is replayed and
   the memory structure is rebuilt. This is the *write-ahead log*.
2. **Immutable files pile up forever.** Nothing is ever cleaned up in place, so dead
   versions and tombstones accumulate and reads have more and more files to check. A
   background thread merges old files together and drops what is no longer needed. This is
   *compaction*.
3. **The engine forgets which files it owns.** Which files exist, how old each one is and
   what keys it covers all live in memory too, and die with it. So that knowledge is
   recorded in a second append-only log, the *manifest*.

Together this is a log-structured merge tree, and the honest summary of it is: cheap writes,
paid for with more expensive reads, which are then bought back with Bloom filters and
compaction.

## Quick start

Everything builds and runs inside a container, and that is deliberate rather than
fashionable. Almost every correctness property here is a claim about the operating system:
whether `fsync` really flushed, whether `rename` is atomic, whether a directory needs its
own `fsync`, whether `flock` actually excludes another process. macOS and Linux disagree on
several of those, so the target is pinned to one Linux image instead of whatever the
developer happens to be sitting in front of.

The only prerequisite is Docker. The image carries GCC 13, CMake, Ninja, GDB, Valgrind and
perf, so there is nothing else to install.

```bash
git clone https://github.com/alladikarthik02/High-Performance-Concurrent-Storage-Engine.git
cd High-Performance-Concurrent-Storage-Engine

./scripts/dev.sh ./scripts/check.sh
```

`dev.sh` runs any command inside the container with the repository mounted, so edits stay on
your machine while compilation happens on Linux. The first run builds the image, which takes
a few minutes; afterwards it is instant.

`check.sh` is the gate. It builds the project three separate times and runs the entire test
suite in each one:

| Build | What it is for |
|---|---|
| `build-none` | plain `-O2`. This is the build that gets benchmarked. |
| `build-address` | AddressSanitizer and UndefinedBehaviorSanitizer: use-after-free, buffer overruns, unaligned loads, signed overflow. |
| `build-thread` | ThreadSanitizer: data races and lock-order inversions. |

Three builds rather than one because AddressSanitizer and ThreadSanitizer cannot live in
the same binary. All three have to pass before any change is considered done.

To run smaller pieces:

```bash
./scripts/dev.sh ./build-none/test_wal     # a single suite
./scripts/dev.sh ./build-none/bench --bench fillrandom --num 10000 --db /data/x
./scripts/dev.sh                           # an interactive shell inside the container
```

## Using it

The engine is a library you link into a program, not a server you talk to over a socket.

```cpp
#include "lsmeng/db.h"

using namespace lsmeng;

Options options;
options.create_if_missing = true;

DB* db = nullptr;
Status s = DB::Open(options, "/data/mydb", &db);
if (!s.ok()) { /* s.ToString() explains what went wrong */ }
```

A write takes an option that decides how much durability you are paying for. With
`sync = false` the record reaches the log and the memtable, which survives the process being
killed but not the machine losing power. With `sync = true` the engine waits for the disk to
confirm before it returns, which is slower but survives both.

```cpp
WriteOptions wo;
wo.sync = true;
s = db->Put(wo, "user:42", "karthik");

std::string value;
s = db->Get(ReadOptions(), "user:42", &value);
if (s.IsNotFound()) { /* never written, or deleted */ }
```

A batch is a group of changes that must either all happen or none happen. That guarantee is
free here, because a whole batch is written as a single record with a single checksum, so
there is no way for a crash to apply half of it.

```cpp
WriteBatch batch;
batch.Put("a", "1");
batch.Delete("b");
s = db->Write(WriteOptions(), &batch);
```

Keys are kept in sorted order, so you can scan a range rather than looking up one key at a
time:

```cpp
Iterator* it = db->NewIterator(ReadOptions());
for (it->Seek("user:"); it->Valid(); it->Next()) {
  // it->key(), it->value()
}
delete it;
```

A snapshot freezes a consistent view of the database while other threads keep writing. It is
wrapped in an RAII handle on purpose: a snapshot that is never released stops compaction
from reclaiming anything, and the database looks perfectly healthy while it silently stops
freeing space.

```cpp
{
  SnapshotHandle snap(db);
  ReadOptions ro;
  ro.snapshot = snap.get();
  s = db->Get(ro, "user:42", &value);
}   // released here

s = db->Close();
delete db;
```

Nothing throws. Every operation that can fail returns a `Status` carrying a machine-readable
code, because in a storage engine a missing key or a full disk is an ordinary outcome rather
than an exceptional one.

## How it fits together

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

Data only ever moves downward through that picture, which turns out to matter for reads.

**Writing.** Threads that want to write join a queue under a single mutex, and whichever
thread is at the front becomes the leader. The leader collects every waiting writer's batch
into one, then releases the mutex and does the expensive part — the log append, the `fsync`,
the inserts — with no lock held at all, before taking the mutex again briefly to publish
sequence numbers and wake everyone it carried. The point is that one `fsync` commits many
writers' data, and the lock only ever protects bookkeeping, never I/O.

**Reading.** A lookup searches newest to oldest: the live memtable, then the frozen one,
then the files tier by tier, stopping at the first match. Because data only moves downward,
the first copy found is always the newest, so no comparison of timestamps is needed; the
search order has already answered the question. If that first copy happens to be a
tombstone, the answer is "not found".

Searching a file would be expensive if done naively, so each one is built to be searched
almost entirely in memory. Four checks run in order, each cheaper than the next and each
eliminating most of what the next would have to do:

```
file's key range (from the manifest) → Bloom filter → block index → read ONE block
         free, file never opened        nanoseconds     in memory      the only disk I/O
```

**In the background.** A single thread writes frozen memtables out as new files in tier 0,
and merges the oldest files of a full tier down into the next one, discarding superseded
versions and expired tombstones on the way. If writers outrun it, they are first slowed by
a millisecond each and then blocked outright, so the database refuses work it cannot sustain
instead of quietly degrading.

**Starting up.** `CURRENT` names the live manifest. Replaying the manifest rebuilds the
knowledge of which files exist and what is in them, which in turn says which logs still
matter; replaying those logs rebuilds the memtable. Then the database opens.

## Design decisions worth explaining

**Why a skip list rather than `std::map`.** Not for speed — a balanced tree searches just as
fast. It is because of what happens during an insert. `std::map` is a red-black tree, and
when it rebalances it *rotates*, rearranging parent and child pointers, and for a moment in
the middle of a rotation it is not a valid tree. A reader walking through at that instant
would follow a broken pointer, and the only defence is to make every read take a lock.
Inserting into a skip list only allocates a node and points existing pointers at it, so
nothing is ever rearranged and there is no bad moment to catch it in. A reader either sees
the new node or it does not, and both are correct answers. That is what allows readers to
search a live memtable with no lock at all. The remaining subtlety is memory ordering: the
writer publishes each pointer with a release store and readers follow with acquire loads, so
seeing a pointer guarantees seeing the finished node behind it rather than a half-built one.

**Why group commit, and where it stops helping.** An `fsync` costs the same whether it is
committing one write or twenty, so it is worth sharing. Writers queue, one of them commits
the whole group, and throughput goes up while the number of `fsync` calls goes down. The
honest other half: with `sync = false` there is no expensive operation to share, every write
still funnels through one leader, and throughput actually *falls* as threads are added.
Group commit amortises a fixed cost; where that cost is absent it is pure overhead.

**Why a lock is never held across file I/O, and how that is enforced.** Holding a mutex
during a disk write is not a data race and not a lock-order inversion, so no sanitizer
reports it. It never produces a wrong answer either, so no test can fail because of it. The
only symptom is worse tail latency months later. A rule that nothing can detect is a rule
that quietly rots, so it is checked at runtime instead: the mutex sets a bit in a
thread-local mask while held, and every call that can block on the filesystem asserts that
the bit is clear. It caught two real violations.

**Why reads need no lock and still cannot be freed underneath.** A read takes the mutex once
to capture its sequence number and take a reference on the memtable, the frozen memtable and
the current set of files, all in the same moment, then releases it and does every bit of
searching and disk reading unlocked. A compaction finishing in the meantime installs a *new*
set of files rather than editing the old one, and the old one stays alive until the last
reader lets go of it.

**Why deleting is harder than writing.** A delete appends a tombstone, which hides older
copies of the key. The difficulty is deciding when the tombstone itself can go. Dropping an
old version is safe once the version that replaced it is visible to the oldest live
snapshot. Dropping a tombstone is only safe once no file outside the current merge, in the
same tier or deeper, could still hold an older copy. The obvious shortcuts for both rules
are wrong in the same quiet way: they resurrect deleted keys, with no crash and no error, and
you find out much later.

**Why everything on disk is checksummed.** Every log record, manifest record and file block
carries a CRC32C that is verified before the bytes are used, with three implementations
chosen at runtime depending on what the CPU supports. One detail is worth calling out: the
CRC of an empty message is zero, so a region of zeros — which is exactly what a file looks
like where space was reserved but never written — would otherwise read back as a perfectly
valid empty record, forever. Two separate defences stop that, because the failure is silent.

## Results

These are measured, not estimated, and `docs/BENCHMARKS.md` carries the exact command behind
each one.

| | Result |
|---|---|
| Group commit, 1 → 16 threads, durable writes | 6.9× more throughput while the number of `fsync` calls *falls* 8.5× |
| One thread, durable writes | within 0.4% of a bare append-plus-`fsync` loop, while maintaining a full sorted index |
| Bloom filters, lookups for absent keys | 38× fewer data blocks read (261,128 → 6,819), 2.4× throughput; measured false-positive rate 0.836% against 0.82% in theory |
| Sharding the block cache, 1 → 16 shards | 1.58× throughput, p90 latency 29 µs → 10 µs |
| Removing two allocations per lookup | +41% throughput, and run-to-run variation dropped from 24% to 1.4% |
| Correctness against a `std::map` reference | 30,000 random operations, 299 compactions, 592 reopens, zero mismatches |
| helgrind, DRD, massif | 0 errors, 0 errors, peak heap 11.55 MiB against a 12 MiB budget |

The second row is the one worth reading twice: it is a ratio against a floor measured on the
same machine, which says the indexing costs almost nothing beyond the durability barrier
itself. The fifth row matters for a different reason — a benchmark that moves 24% between
identical runs cannot support any claim about tail latency, so cutting that noise made every
other measurement mean something.

Reproducing the headline write benchmark:

```bash
./scripts/dev.sh ./build-none/bench --bench fillrandom --threads 16 --num 40000 \
                                    --sync true --db /data/ws
```

## How it is tested

The bugs that matter in a storage engine mostly do not crash. They return a wrong answer,
later, under load, which shapes how the testing works.

The primary net is **differential testing against `std::map`**. The engine and a plain
`std::map` are driven with the same randomised mix of puts, deletes, batches, flushes,
compactions, reopens and scans, and compared after every single operation. The map is too
simple to be wrong, so it serves as the definition of the right answer. This matters because
the resurrection bugs described above require a specific coincidence of snapshot timing,
tier depth and which files happen to be merged together; random generation produces those
coincidences in shapes nobody would think to write by hand.

Durability is tested through a **pluggable filesystem layer** that can simulate a crash by
dropping writes that were never `fsync`ed, along with file creations and renames that were
never made durable. That is an assertion a simple `kill -9` test cannot make.

Recovery is tested **exhaustively rather than by sampling**: the log is truncated at every
byte offset in turn, and a flipped bit is walked through every byte position, asserting each
time that recovery returns a clean prefix and never a record with wrong contents.

Every randomised test prints its seed and honours `LSMENG_SEED`, so any failure can be
replayed exactly.

## Configuration

All defaults live in `include/lsmeng/options.h`.

| Option | Default | What it controls |
|---|---|---|
| `write_buffer_size` | 4 MiB | how large the memtable grows before it is frozen and flushed |
| `block_size` | 4 KiB | target size of a data block, matched to a disk page |
| `block_restart_interval` | 16 | how often a full key is stored, trading file size against lookup cost |
| `bloom_bits_per_key` | 10 | filter accuracy; 0 disables filters entirely |
| `block_cache_bytes` | 8 MiB | memory budget for cached data blocks |
| `filter_memory_bytes` | 16 MiB | memory budget for resident Bloom filters |
| `cache_shards` | 16 | how many independent locks the block cache is split across |
| `tier_trigger` | 4 | how many files a tier holds before it is compacted |
| `max_tiers` | 7 | beyond this the deepest tier merges into itself instead of growing |
| `tier0_slowdown_trigger` | 8 | tier-0 file count at which writes take a 1 ms brake |
| `tier0_stop_trigger` | 12 | tier-0 file count at which writes block entirely |
| `paranoid_checks` | true | verify a checksum on every block read |

## Tools

```bash
./build-none/sst_dump <file.sst> [--entries]   # footer, index, filter and block contents
./build-none/env_facts                         # what this machine and filesystem actually do
./build-none/bench --bench <name> ...          # fillseq, fillrandom, readrandom, readhot,
                                               # readmissing, mixed, baseline
```

`env_facts` exists because several design decisions depend on facts about the environment
rather than assumptions about it — how long an `fsync` really takes, whether `flock` really
excludes, which CRC32C instruction the CPU has.

## Repository layout

```
include/lsmeng/   public and internal headers; the reasoning lives in these comments
src/              the engine
tests/            22 suites, the test harness, and the fault-injecting filesystem layer
bench/            benchmark harness with latency histograms and an open-loop mode
tools/            sst_dump, env_facts
scripts/          dev.sh runs anything in the container, check.sh is the full gate
docs/             specification, bug journal, benchmarks, claims, safety audit
```

## Known limitations

These are stated rather than buried, and most of them are deliberate trades.

**Reads touch more places than a B-tree would.** A key could be in any file, so a lookup may
check several. Filters and indexes reduce that considerably but never remove it. On a
read-heavy workload a B-tree is the better design, and that was a conscious choice.

**Every byte is written more than once.** Once into the log, again into a file when the
memtable is flushed, and again each time compaction moves it down a tier. That is the price
of never doing a random write.

**There is one background thread**, handling both flushes and compactions, and a running
compaction is never interrupted. So a long merge deep in the tiers can delay an urgent
tier-0 flush. This was chosen on purpose: one background thread means only one thread ever
writes the manifest, which removes an entire class of race rather than guarding against it.

**Durability is verified against process crashes, not power loss.** The tests kill the
writing process and assert that nothing acknowledged was lost, and separately simulate lost
un-`fsync`ed writes. Power-loss durability is not claimed, because `fsync` inside a container
on Docker Desktop for macOS crosses a virtualisation boundary and that chain is not a
hardware guarantee.

**One process per database directory**, enforced with `flock` plus a pid-based fallback for
filesystems where `flock` does not actually exclude.

**Blocks are not compressed.** The format reserves a byte for it and the reader rejects
anything it does not recognise, but only uncompressed blocks are written. Keys *are*
prefix-compressed within a block, which is a different mechanism.

**Compaction is count-triggered tiering**, not size-bucketed like Cassandra's. On a workload
that overwrites a small key set forever, equally sized files can accumulate in different
tiers without ever being paired; `max_tiers` bounds the damage, and size-bucketed selection
is the named next step.

## Documentation

| Document | What is in it |
|---|---|
| [`docs/SPEC.md`](docs/SPEC.md) | the design: architecture, API, 25 safety requirements, 39 edge cases anticipated in advance, and a review log of 21 defects found by attacking the first draft on purpose, four of which would have silently lost data |
| [`docs/CHALLENGES.md`](docs/CHALLENGES.md) | a bug journal kept while building, with the wrong hypotheses preserved rather than only the fixes |
| [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) | every measurement, with the command line that produced it |
| [`docs/CLAIMS.md`](docs/CLAIMS.md) | each claim about this engine mapped to the evidence that earns it |
| [`docs/SAFETY-AUDIT.md`](docs/SAFETY-AUDIT.md) | all 25 safety requirements and how each is enforced |

## License

BSD 3-Clause. See [`LICENSE`](LICENSE).
