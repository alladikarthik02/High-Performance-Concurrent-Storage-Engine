# `lsmeng` — résumé bullets mapped to evidence

The two bullets, phrase by phrase, each against the thing that earns it. Every number here
appears in `docs/BENCHMARKS.md` with the command line that produced it.

**The rule this document exists to enforce:** if a phrase has no measurement, the phrase
gets rewritten. The résumé follows the code, not the other way around.

---

## Bullet 1

> *"Built a C++ storage engine using an LSM-tree design with a write-ahead log for
> durability, sustaining high write throughput while keeping reads fast through Bloom
> filters and a tiered compaction strategy."*

### "Built a C++ storage engine"

**6,709 lines of C++20 in the engine, plus 5,695 lines of tests, benchmarks and tools.**
Zero third-party libraries — the only things linked are `libstdc++` and `pthread`. No
RocksDB, no LevelDB, no Snappy, no gtest, no Abseil. `CMakeLists.txt` contains no
`find_package`.

*If asked "what did you actually write?"* — the WAL framing and its recovery, the skip
list and its memory ordering, the SST block format with prefix compression and restart
points, the Bloom filter, the k-way merge, the compaction drop rules, the MANIFEST and
version set, the sharded LRU cache, CRC32C with three runtime-selected implementations, the
comparator, and the latency histogram.

### "using an LSM-tree design"

Writes land in a skip-list memtable, are flushed to immutable sorted files, and are merged
downward by a background thread. **No data file is ever modified in place.**

- Memtable: `include/lsmeng/skiplist.h`, `memtable.h` — measured **1.28× arena overhead**
  over user bytes, correcting the spec's assumed 2–3×.
- SST: `include/lsmeng/sst.h`, `src/sst.cc` — data blocks, a whole-file Bloom block, an
  index block, and a fixed 48-byte footer with the magic **last**, so an interrupted write
  cannot produce a parseable footer.
- `sst_dump` exists to read any of it off disk.

### "with a write-ahead log for durability"

`include/lsmeng/wal.h`. Record = `crc32c(4) | len(4) | type(1) | payload`, with the CRC
computed over `(log_number ‖ len ‖ type ‖ payload)`.

**Measured: 0 of 300 acknowledged `sync=true` writes lost after a simulated crash.**

Two details worth being able to explain unprompted:

- **A zero-length record is invalid by construction, and the CRC is seeded with the file's
  log number.** `CRC32C("")` is `0x00000000`, so `crc=0, len=0` is a self-consistent
  record — and a preallocated-but-unwritten extent is exactly that. Without both defences a
  zero-filled hole parses as an unbounded stream of "valid" empty records. Tested with a
  hand-crafted 4 KiB hole.
- **Two logs are live at once**, from the moment a memtable is frozen until its flush has a
  durable MANIFEST edit. Recovery replays every log at or above `min_live_log` in ascending
  order, with a **per-file** torn-tail rule. The spec's first version had one log number and
  would have silently discarded every acknowledged write since the last switch.

Recovery is tested by truncating a log at **every one of 87 byte offsets** and asserting a
clean prefix each time, and by flipping a bit at every byte position and asserting no
record with wrong contents is ever returned.

### "sustaining high write throughput"

**High compared to what** — the question an interviewer asks, answered in advance with
three baselines (`SPEC §2.1`).

| threads (`sync=true`) | ops/s | fsyncs | **writes per fsync** |
|---|---|---|---|
| 1 | 3,031 | 40,000 | 1.00 |
| 8 | 12,233 | 8,915 | 4.49 |
| **16** | **20,950** | **4,718** | **8.48** |

**6.9× throughput from 1 to 16 threads while the fsync count falls 8.5×.** That is
leader/follower group commit: one `fsync` commits many writers' data, and a writer performs
a WAL append only while it is at the head of the queue.

The number to quote is the ratio, not the raw figure: **at one thread the engine sustains
3,031 ops/s against a raw-`write`-plus-`fsync` floor of 3,043 — 99.6% of the floor, while
maintaining a full sorted index.**

**And the honest counterpoint, which is worth volunteering:** with `sync=false` throughput
*falls* with thread count (1.05M → 169k from 1 to 8 threads). Group commit amortises an
expensive operation; where there is no `fsync` to amortise, the serialisation is pure cost.

### "keeping reads fast through Bloom filters"

Own implementation, Kirsch–Mitzenmacher double hashing off one 64-bit hash with `h2` forced
odd.

**Measured false-positive rate: 0.836% against 0.82% theory** at 10 bits/key — within 2%.

End to end, 400,000 lookups for absent-but-in-range keys:

| bits/key | data blocks read | throughput |
|---|---|---|
| 0 (off) | 261,128 | 1.13M ops/s |
| **10** | **6,819** | **2.72M ops/s** |

**38× fewer data blocks read, 2.4× throughput.** The implied end-to-end FPRs (15.6% / 0.83%
/ 0.04% at 4 / 10 / 16 bits) match the unit-test FPRs almost exactly — two independent
measurements of the same quantity agreeing.

Three rules behind it that are each a data-loss bug if broken: **tombstones go in the
filter** (omitting a deletion's key makes the file skippable and resurrects the value
beneath it); nothing ever clears a bit; and a **missing or corrupt filter degrades to
"maybe present", never "not present"** — failing open costs a read, failing closed loses
data.

### "and a tiered compaction strategy"

The **T oldest** files of a tier merge into one file in the next tier. Three things to be
able to defend:

1. **Inputs must be an oldest-first prefix**, never a size- or newest-chosen subset. The
   read path's correctness rests on an invariant — *for every user key, the shallowest tier
   containing it holds its newest version* — and that induction only closes if everything
   moved down was written before everything left behind.
2. **A tombstone may be dropped only when no non-input file in the output tier or deeper
   could hold an older value.** `is_bottom_tier` is *not* that test: tiered compaction
   writes into a tier that already holds older files the merge never reads. LevelDB's
   shortcut does not transfer, because leveled compaction pulls the overlapping
   output-level files into its inputs and tiered does not.
3. **The dedup rule is about the previously *emitted* version, not the current one.**
   Testing the entry's own sequence drops precisely the version an old snapshot needs —
   every time, because the first version at or below the boundary always satisfies it.

**Measured: 6,000 writes over 500 distinct keys leaves 1,320 entries on disk.** If the drop
rule regressed, it would be 6,000 and nothing else would change.

Tier count is bounded by `max_tiers`: after 40 overwrite rounds with `max_tiers=4`, nothing
exists beyond tier 3. And the honest limitation, stated in the spec: **this is
count-triggered tiering with an oldest-prefix rule, not Cassandra-style size-bucketed STCS.**
Size bucketing is the named next step; it would need its own proof that it preserves the
ordering invariant.

---

## Bullet 2

> *"Optimized the concurrent path with fine-grained locking and benchmarked it under
> multithreaded load, profiling with perf and Valgrind to remove contention and reduce
> tail latency at scale."*

### "the concurrent path"

Any number of application threads plus **exactly one** background thread that flushes first
and compacts second. Readers, writers, a live flush and a live compaction all run
simultaneously.

**Measured: 82,279 concurrent reads with 0 inconsistent** during live compaction; TSan
clean; helgrind and DRD both report **0 errors** on the same workload.

*Why one background thread and not two* — a real design decision with a stated price. Two
threads both appending `VersionEdit`s to one MANIFEST, with no lock, interleave bytes
mid-record; the CRC then fails at the splice and the torn-tail rule silently discards both
edits and every later one. One thread removes the race rather than guarding it, and makes
on-disk record order equal in-memory install order by construction. The price is
head-of-line blocking — a deep merge delays tier-0 drainage — which is documented and made
observable (`lsmeng.compaction-in-progress`, `max-stall-ms`) rather than hidden.

### "with fine-grained locking"

A written lock inventory, because the follow-up question is *"what locks, in what order, and
what did you avoid holding them across?"*

| | lock | protects | held across I/O? |
|---|---|---|---|
| L1 | `db_mutex_` | writer queue, memtable pointers, version pointer, snapshot list, compaction state | **No** |
| L2 | block-cache shard mutex ×16 | one shard's table + LRU | No |
| L3 | table-cache shard mutex ×16 | open `SstReader`s and their pinned filters | No |
| L4 | per-writer condvar | one writer's `done` flag | n/a |
| L5 | `bg_cv_` | background job state, stalls, shutdown | n/a |

Order: `L1 → (L2 | L3)`, never the reverse — **confirmed by helgrind, which found no
lock-order inversion on a live database.**

**And what is deliberately *not* locked, with the reason** — which is the more interesting
half:

- **The skip list takes no lock at all.** One writer (guaranteed by the group-commit head-of-
  queue rule), any number of readers, `store(release)` / `load(acquire)` on the forward
  pointers. Nothing is ever deleted from a memtable, so there is no reclamation problem and
  therefore **no hazard pointers and no RCU** — being able to say *why they are unnecessary*
  is the point.
- **A `Version` is immutable and refcounted.** A reader pins one under `db_mutex_` and then
  does all its I/O with no lock. Files are immutable and unlinking an open fd on POSIX leaves
  it readable, so a badly-timed unlink cannot corrupt an in-flight read.
- **`pread`, never `read`** — `read` advances an offset shared by every thread on that
  descriptor, and the resulting garbage looks exactly like an SST checksum failure. The `Env`
  interface exposes only positional reads, so the mistake is unavailable.

**The claim is enforced by a machine, not by prose.** Holding a lock across I/O is neither a
data race nor a lock-order inversion, so TSan, helgrind and DRD all report nothing. So
`db_mutex_` is a `TrackedMutex` that sets a thread-local bit, and every blocking `Env` entry
point asserts the bit is clear. **It caught two real violations in code written while
thinking about the rule** — a MANIFEST `fsync` under the mutex, and opening every SST under
it while building an iterator.

### "benchmarked it under multithreaded load"

`bench/bench.cc` — thread counts from 1 to 16, closed and open loop, reporting **p50 / p90 /
p99 / p99.9 / max**, never a mean, from an own log-bucket histogram verified against a
sorted array (never overstates a percentile; understates by at most one bucket width).

**Coordinated omission is handled as a protocol, not a flag.** A closed-loop benchmark
cannot observe a stall correctly — while the engine is stuck the loop simply issues fewer
requests. Open loop measures from each operation's *intended* start time, but is only
meaningful below saturation, so: calibrate `M` closed-loop, target fractions of it, and
**refuse to print percentiles for a run that achieves under 99% of target.**

```
rate=6460  -> valid: 100.0% of target, p99.9 = 6,768 us
rate=12000 -> *** INVALID -- DIVERGED (59% of target). Percentiles NOT reported.
```

An earlier build of the harness happily reported `p99.9 = 864 ms` for that diverging run —
a number that is a function of run *duration*, not of the engine. Being able to explain why
that is wrong is worth more than any figure above it.

### "profiling with perf and Valgrind"

**`perf`:** hardware counters are unavailable — the container runs inside a LinuxKit VM with
no exposed PMU, so `cycles` and `instructions` report `<not supported>`. **The spec predicted
this before any code existed and named the substitutes.** Profiles use `perf record -e
cpu-clock`, a software event that works under virtualisation.

**Valgrind:** cachegrind (a cache *simulator*, immune to the missing PMU — D1 miss rate
1.4%, LL essentially 0%), helgrind and DRD (0 errors on the live database), massif (**peak
heap 11.55 MiB against a declared 12 MiB budget**, which is the evidence for the
no-unbounded-growth requirement), and memcheck on every suite in the ASan/LSan configuration.

The honest framing: *"perf's hardware counters don't work under Docker Desktop's VM, so the
profiles are `cpu-clock` software sampling plus cachegrind simulation — which is arguably the
better tool for cache behaviour anyway."*

### "to remove contention"

**Two named changes, each found by reading a profile.**

**(1) Sharding the block cache.** An LRU is *mutated on every hit* — the entry moves to the
list head — so even a pure-read workload serialises on one mutex. 1 → 16 shards:
**1.58× throughput, p90 29 µs → 10 µs.**

**(2) Removing allocation from the read path.** The profile said something the spec had not
anticipated: in the configuration deliberately set up to be lock-bound, **allocation cost
more than lock contention — 13.7% of cycles against 8.0%.** Two causes, both on a cache
*hit*: the cache's hash lookup built a `std::string` **inside the shard mutex**, and
`SstReader::Get` heap-allocated three iterator objects that never outlived the call. Fixed
with transparent heterogeneous lookup and a stack-allocated point-lookup path.

| | before | after |
|---|---|---|
| throughput (1 shard) | 503k ops/s | **710k ops/s** (+41%) |
| p90 | 42 µs | **30 µs** (−29%) |
| run-to-run spread | 24% | **1.4%** |

**And where the bottleneck moved**, which is the part that shows the profiler was read
rather than run: allocation is *still* the largest group (15.7%), but it has moved off the
point-lookup path onto the **scan** path, where iterators genuinely must outlive their
creating call. That is a harder problem and it is the named next step.

### "and reduce tail latency at scale"

"At scale" here means **8–16 threads on 8 cores**, stated plainly rather than implied.

| change | p90 | p99 | p99.9 |
|---|---|---|---|
| cache sharding 1 → 16 | 29 → 10 µs | 131 → 99 µs | 240 → 295 µs *(no improvement)* |
| allocation removal (1 shard) | 42 → 30 µs | 160 → 127 µs | — |

**The most useful thing in this table is the row that did not improve.** Sharding does
nothing for p99.9, so the deep tail is not the cache lock. Under the mixed workload — 2
writers, 8 readers, 42 compactions live — p99.9 is 449 µs against a **128 ms maximum**, and
that gap is compaction. The write stall is designed behaviour (bounded tier-0 growth, or
reads degrade without limit), it is measured (`lsmeng.stalls`, `lsmeng.max-stall-ms`), and
**it is what a two-worker compaction pool would attack next.**

---

## What this project is not

Volunteering the limits is what makes the claims above credible.

- **Not a database server.** No network, no replication, no consensus. (The Raft/gRPC work
  is a different project and is described as one.)
- **`fsync` here is not a power-loss guarantee.** Docker Desktop on macOS routes writes
  through a VM and a shared-filesystem driver — and the bind mount's `fsync` measured **6×
  faster** than the container filesystem's, which is the alarming direction and suggests it
  is not really flushing. Every durability test runs on the container-local volume. What is
  verified is process crash-consistency (`kill -9`) plus a fault-injecting `Env` that drops
  un-`fsync`ed bytes — because a `kill -9` test alone would pass with every `fsync` deleted.
- **Count-triggered tiering, not size-bucketed STCS**, with `max_tiers` bounding the damage.
- **No block compression**, no secondary indexes, no cross-key transactions beyond a batch,
  one process per directory.

## The documents worth reading before an interview

| | |
|---|---|
| `docs/SPEC.md` | the design, and **§11: 21 defects found by attacking version 1 on purpose**, four of which would have silently lost data |
| `docs/CHALLENGES.md` | the bug journal — C1–C3 from the design phase, **B1–B21 from the build**, each with the wrong hypotheses kept |
| `docs/BENCHMARKS.md` | every number, with the command line that produced it |
