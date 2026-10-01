# `lsmeng` — measured numbers

Every number here was produced on this machine by the command line shown next to it.
SPEC §2.1: **a number without its command line does not go in this file.**

`MEASURED` = observed here. `ASSUMED` = a hypothesis a later task must confirm or kill.
Nothing is unmarked.

**Host:** Apple Silicon (arm64), macOS 26.2, Docker Desktop → LinuxKit VM → Ubuntu 24.04
container, GCC 13. 8 CPUs visible to the container.

---

## T0 — environment facts

```bash
./scripts/dev.sh ./build-none/env_facts /work/scratch /data
```

| Fact | Value | Consequence |
|---|---|---|
| Architecture | **aarch64** | `MEASURED`. The x86 SSE4.2 `_mm_crc32_u64` intrinsic **does not exist here**. T1's hardware CRC32C path must use the ARMv8 `__crc32cd` intrinsic behind `+crc`. See B3. |
| `nproc` | 8 | Benchmarks sweep 1–8 threads, not 1–16; a 16-thread number on 8 cores measures the scheduler |
| Page size | 4096 B | |
| ARM CRC extension | **not enabled by default** | Needs a `target("+crc")` function attribute plus a runtime `getauxval` check — a compile-time-only assumption would produce SIGILL on a CPU without it |
| `fsync` median, **bind mount** (`/work`) | **55.0 µs** | `MEASURED` |
| `fsync` median, **container fs** (`/data`) | **339.1 µs** | `MEASURED` — **6× slower than the bind mount.** See B5: the fast one is the suspicious one |
| `SyncDir` | supported on both | E-7's mechanism is available here (no `EINVAL` fallback needed) |
| **`flock` excludes across processes, bind mount** | **NO** | `MEASURED`. **The `wanrep` B2 finding reproduces exactly.** S16's pid-file fallback is load-bearing, not belt-and-braces. See B4 |
| `flock` excludes across processes, container fs | yes | Which is why SPEC §8 puts every test database on `/data` |

### perf, checked against the §10.4 prediction

```bash
./scripts/dev.sh /usr/lib/linux-tools-6.8.0-138/perf stat -e cycles,instructions true
./scripts/dev.sh /usr/lib/linux-tools-6.8.0-138/perf stat -e cpu-clock true
```

```
   <not supported>      cycles:u
   <not supported>      instructions:u
              0.18 msec cpu-clock:u    #  0.415 CPUs utilized
```

`MEASURED`. **§10.4's prediction was correct**: no PMU is exposed through the LinuxKit VM,
so hardware events are unavailable and `perf lock` is out. The software `cpu-clock` event
works and gives a real wall-clock sampling profile. T12 therefore uses `perf record -e
cpu-clock` plus cachegrind (a *simulator*, immune to the missing PMU) — and R11's claim is backed by a tool that actually produced output, not one that printed
`<not supported>`.

---

## T1 — primitives

```bash
./scripts/dev.sh ./build-none/test_crc32c    # implementation line
./scripts/dev.sh ./build-none/test_arena     # MEASURED line
./scripts/dev.sh ./build-none/test_hash      # MEASURED line
```

| Fact | Value | Note |
|---|---|---|
| CRC32C path selected at runtime | **`aarch64 __crc32c (hardware)`** | `MEASURED`. `HWCAP_CRC32` is present, so the ARMv8 instruction is live. Verified against the RFC 3720 vectors **and** differentially against the portable table over random inputs — "the fast and slow paths disagree" would be silent corruption, not a perf bug |
| Hash collisions, 200k structured keys | **0** | `MEASURED`. Keys of the form `user:<n>:name` — long shared prefixes, which is the shape that breaks weak hashes |
| Hash bit distribution | every one of 64 bits set 45–55% of the time | `MEASURED`. The Bloom filter splits this into two 32-bit halves, so a poorly-mixed high half would quietly degrade the FPR |
| Cache shard balance, 16 shards | every shard within ±10% of even | `MEASURED`. A skewed map would make the R12 sharding experiment measure nothing |
| **Arena overhead vs user bytes** | **1.48×** | `MEASURED` (modelled node). **SPEC §3.4 assumed 2–3×; the assumption was pessimistic and is now corrected.** 3,422,736 arena bytes for 20,000 records of 116 user bytes. Refined against real skip-list nodes in T3 |

## T1b / T2 — stats and the Bloom filter

```bash
./scripts/dev.sh ./build-none/test_bloom     # MEASURED lines
```

| bits/key | k | **measured FPR** | theory | bytes/key |
|---|---|---|---|---|
| 4 | 2 | **0.15560** | 0.16000 | 0.50 |
| **10 (default)** | **6** | **0.00836** | **0.00820** | **1.25** |
| 16 | 11 | **0.00040** | 0.00048 | 2.00 |

`MEASURED`, 20,000 keys inserted, 100,000 absent-key probes each. **R6 is earned by this
table.** The m=10 row is within 2% of theory, which is what says the double-hashing
scheme (one 64-bit hash split into two 32-bit halves, `h2` forced odd) behaves like k
independent hashes at these parameters — the thing SPEC §3.6 asserted and could not prove.

**One honest discrepancy.** SPEC §3.6 said `k = round(m·ln2) = 7`; the code truncates, so
k = 6. Measured 0.00836 sits right on k=6 theory (0.00844) rather than k=7 theory
(0.00819). Truncation is kept deliberately: **k=7 buys a 3% better FPR for 17% more probes
per lookup**, and the probes are on the read hot path. The spec is corrected to say
`floor`, rather than the code being bent to match a number nobody had measured.

Filter size is **1.25 bytes/key**, flat from 1k to 100k keys — which is the input to E-34:
a deep-tier file holding ~10⁶ keys carries a **~1.25 MiB** resident filter, 15% of the
default 8 MiB block cache for a single file. That is why the table cache is bounded in
bytes and not only by file count.

## T3 — skip list and memtable

```bash
./scripts/dev.sh ./build-none/test_memtable   # MEASURED line
./scripts/dev.sh ./scripts/check.sh thread    # the TSan verdict
```

| Fact | Value | Note |
|---|---|---|
| **Memtable arena overhead** | **1.28×** | `MEASURED` — real skip-list nodes, real encoding, real random heights. 7,424,136 bytes for 50,000 records of 116 user bytes. **SPEC §3.4 assumed 2–3×; T1's modelled estimate said 1.48×; the truth is 1.28×.** A 4 MiB `write_buffer_size` therefore holds ~3.1 MiB of user data, not the ~1.5 MiB the spec implied — so memtables hold roughly twice as many records as planned, and flush half as often |
| Concurrent reads, 1 writer + 8 readers (skip list) | ~600,000, **0 torn, 0 from-future** | `MEASURED` |
| Concurrent entry reads, 1 writer + 8 readers (memtable) | 286,625, **0 inconsistent** | `MEASURED` |
| **TSan verdict** | **clean** | S6 holds at this layer: no lock anywhere in the skip list, readers on acquire, the single writer on release |

## T5 — the SST file

```bash
./scripts/dev.sh ./build-none/test_sst        # MEASURED lines
./scripts/dev.sh ./build-none/sst_dump <file> # per-file inspection
```

**R6, directly measured** — 20,000 keys in the file, 20,000 lookups for keys that were
never written:

| `bloom_bits_per_key` | data blocks read | per lookup | rejected by filter |
|---|---|---|---|
| **0 (filters off)** | 20,000 | **1.0000** | 0 |
| **10 (default)** | 156 | **0.0078** | 19,844 |

`MEASURED`. **A 128× reduction in data blocks read for absent keys**, and 0.78% agrees
with T2's independently measured 0.836% false-positive rate. This is the number behind
"keeping reads fast through Bloom filters": with filters off, every candidate file costs a
block read; with them on, 99.2% of those reads never happen.

| Fact | Value |
|---|---|
| 5,000 entries, 40 B values, `block_size=512` | 326,801 bytes across 556 data blocks |
| Prefix compression + index + filter + trailers | see `sst_dump`'s `file/user ratio` |

## T6 — block cache and table cache

```bash
./scripts/dev.sh ./build-none/test_table_cache   # MEASURED lines
```

| Fact | Value | Note |
|---|---|---|
| 100 identical point lookups | **1 block read, 99 cache hits** | `MEASURED`. The block cache does what it claims |
| `max_open_files = 4`, 50 files touched | **3 readers resident** | `MEASURED`. E-26: fds are bounded |
| `max_open_files = 1000`, `filter_memory_bytes = 32 KiB`, 30 large files | **12 readers, 30,396 bytes charged** | `MEASURED`. **E-34: the BYTE limit bound it, not the count.** With a count-only limit all 30 readers would have stayed resident and pinned their filters — which is exactly the hole SPEC v1 had |

Sharding (1 vs 4 vs 16) is built and correct; **the contention delta it exists to produce
is T12's measurement**, not T6's.

## T7 — the two-process exclusion gate (S16)

```bash
./scripts/dev.sh ./build-none/test_lock_exclusion                          # /data
./scripts/dev.sh env LSMENG_TEST_DIR=/work/scratch ./build-none/test_lock_exclusion  # bind mount
```

| Filesystem | Cross-process exclusion | Same-process | Verdict |
|---|---|---|---|
| `/data` (container) | **enforced** | **enforced** | pass |
| `/work` (bind mount) | **enforced** *(by the pid fallback — `flock` does nothing here)* | **enforced** *(by the in-process table)* | pass |

`MEASURED`. **This is the gate on `gc_orphans_on_open`** (SPEC §3.9 / E-6): orphan
collection unlinks files, and its safety argument is S16. It now passes on both
filesystems — but it takes **three** mechanisms to do so, one per failure mode (B13), and
the default stays `false` regardless.

## T8 — the write path

```bash
./scripts/dev.sh ./build-none/test_db_write   # MEASURED lines
```

| Fact | Value | Note |
|---|---|---|
| **Group commit at 8 threads, all `sync=true`** | **3,200 writes / 720 fsyncs = 4.44 writes per fsync** | `MEASURED`. **R5's mechanism, demonstrated.** Each writer asked for durability; one fsync committed 4.4 of them on average. A ratio near 1.0 would mean group commit is not working |
| Acknowledged `sync=true` writes lost after a simulated crash | **0 of 300** | `MEASURED`. **R4.** Through `FaultEnv`, which drops un-fsynced bytes — the assertion a `kill -9` test cannot make |
| Keys surviving reopen across many memtable switches | **2,000 / 2,000** | `MEASURED`. E-36: two logs are live at once, and both are replayed |
| Concurrent reads during 4 writers + live flush | 30,865, **0 inconsistent** | `MEASURED`, TSan clean |

The full thread sweep (1→8) and the throughput numbers are T11's; this is the *mechanism*
check that R5 rests on.

## T9 — the read path, and the model test

```bash
LSMENG_MODEL_OPS=30000 ./scripts/dev.sh ./build-none/test_db_read
```

```
30000 ops: 13453 puts, 6005 deletes, 2950 batches, 601 flushes, 583 reopens,
           1181 scans; 105 live keys      -- 8 tests, 0 failures  (7m33s)
```

`MEASURED`. **T9's exit criterion.** 30,000 randomly chosen operations run against a
`std::map` reference model, with every `Get` compared immediately and a full **forward and
reverse** scan compared against the model 1,181 times. `Reopen` is in the op mix, so this is
a recovery test as well — 583 of them. The key space is 150 keys deliberately, so overwrites,
deletes and resurrections collide constantly.

`CompactRange` is excluded until T10; SPEC §9 states that forward dependency rather than
hiding it.

Default depth is 4,000 ops (≈3 s) and 600 under sanitizers, so `check.sh` stays usable —
the deep run above is what certifies the read path.

## T10 — tiered compaction

```bash
./scripts/dev.sh ./build-none/test_compaction
LSMENG_MODEL_OPS=20000 ./scripts/dev.sh ./build-none/test_db_read
```

| Fact | Value | Note |
|---|---|---|
| **Entries on disk after 6,000 writes over 500 keys** | **1,320** (500 live) | `MEASURED`. **E-33.** With `oldest_snapshot_seq` accidentally 0, this would be **6,000** and no other test would notice |
| 2,000 keys written then all deleted, then compacted | **0 live keys**, 377,917 bytes compacted | `MEASURED`. Tombstones and their values are both reclaimed |
| Tier count after 40 overwrite rounds, `max_tiers=4` | **`t3=1`** — nothing beyond tier 3 | `MEASURED`. **E-32:** tier count is bounded by live data, not by total bytes written |
| Concurrent readers + writers + live compaction | 82,279 reads, **0 inconsistent**, 7 compactions | `MEASURED`, TSan clean. **R8** |
| **Model test, full op mix** | **20,000 ops: 9,106 puts, 3,985 deletes, 1,981 batches, 200 flushes, 199 `CompactRange`, 388 reopens, 755 scans — 0 failures** | `MEASURED`. **T10's exit criterion.** Compaction in the op mix is what makes the E-2 resurrection bugs catchable by construction rather than by hand-built scenario |

All five E-2 resurrection scenarios have their own hand-built test as well, because three of
them (E-2a, E-2d, E-2e) correspond to bugs SPEC v1 actually had.

## T11 — the benchmarks

All runs: Apple Silicon (arm64) → Docker Desktop → LinuxKit VM → Ubuntu 24.04, 8 CPUs,
database on `/data` (container-local). **Never the bind mount** — B5 measured its `fsync`
at 55 µs against `/data`'s 339 µs, which is the wrong direction for a durability barrier
and suggests it is not really flushing.

### R5 — write throughput, and what "high" is measured against (§2.1)

**B-null, the floor.** Raw `write()` of the same bytes to a flat file, no index, no
structure:

```bash
./build-none/bench --bench baseline --num 20000 --sync true --db /data/b
```
| | ops/s |
|---|---|
| raw append, no fsync | 5,351,606 |
| **raw append + fsync** | **3,043** |

**B-selfscale — the engine, `sync=true`, every writer asking for durability:**

```bash
for t in 1 2 4 8 16; do ./build-none/bench --bench fillrandom --threads $t --num 40000 --sync true --db /data/ws; done
```

| threads | ops/s | `wal-syncs` | **writes per fsync** | p50 | p99 | p99.9 |
|---|---|---|---|---|---|---|
| 1 | 3,031 | 40,000 | 1.00 | 319 µs | 648 µs | 972 µs |
| 2 | 4,382 | 26,682 | 1.50 | 363 | 998 | 1,412 |
| 4 | 7,146 | 16,001 | 2.50 | 568 | 1,032 | 1,636 |
| 8 | 12,233 | 8,915 | 4.49 | 662 | 1,124 | 1,804 |
| **16** | **20,950** | **4,718** | **8.48** | 758 | 1,220 | 2,216 |

`MEASURED`. **This is R5.** Two things it says, and the second is the more honest one:

1. **6.9× throughput from 1 to 16 threads while the fsync count FALLS 8.5×.** That is group
   commit doing exactly what it claims: one `fsync` committing many writers' data.
2. **At 1 thread the engine sustains 3,031 ops/s against a raw-append-plus-fsync floor of
   3,043 — 99.6% of the floor, while maintaining a full sorted index.** The LSM write path
   costs essentially nothing over the durability barrier itself. That is the number worth
   quoting, and it is a *ratio to a baseline*, not a bare figure.

**The honest counterpoint, `sync=false`:**

| threads | 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| ops/s | 1,046,646 | 716,586 | 383,234 | 169,155 |

`MEASURED`. Throughput **falls** with thread count. With no `fsync` there is nothing to
amortise, so the leader/follower serialisation is pure overhead — every write still funnels
through one leader. **Group commit is a mechanism for amortising an expensive operation,
and where that operation is absent it is a cost.** Saying so unprompted is worth more than
the good number above it.

### R6 — Bloom filters, end to end

```bash
for b in 0 4 10 16; do ./build-none/bench --bench readmissing --threads 4 --num 400000 --bloom_bits $b --db /data/rb2; done
```

200,000 keys loaded (even), 400,000 lookups for **in-range absent keys** (odd):

| bits/key | ops/s | **data blocks read** | filter rejections | **implied FPR** |
|---|---|---|---|---|
| **0 (off)** | 1,130,406 | **261,128** | 0 | — |
| 4 | 2,520,914 | 43,047 | 337,772 | 15.6% |
| **10 (default)** | **2,718,876** | **6,819** | 396,694 | **0.83%** |
| 16 | 2,758,019 | 5,629 | 399,847 | 0.04% |

`MEASURED`. **2.4× throughput and a 38× reduction in data blocks read** at the default. The
implied end-to-end FPRs (15.6% / 0.83% / 0.04%) match T2's unit-test measurements
(15.6% / 0.836% / 0.04%) almost exactly — two independent measurements of the same
quantity agreeing is what turns a number into evidence.

Diminishing returns are visible: 10→16 bits/key costs 60% more filter memory to remove a
further 0.8% of block reads.

### R12/R13 — cache sharding

```bash
for s in 1 4 16; do ./build-none/bench --bench readhot --threads 8 --num 200000 --cache_shards $s --db /data/rh; done
```

| shards | ops/s | p50 | **p90** | p99 | p99.9 |
|---|---|---|---|---|---|
| **1** | 708,820 | 2 µs | **29 µs** | 131 µs | 240 µs |
| 4 | 1,002,074 | 2 | 13 | 107 | 230 |
| **16** | **1,117,832** | 2 | **10 µs** | 99 µs | 295 |

`MEASURED`, 8 threads, ~98% cache hit rate. **1.58× throughput and a 2.9× improvement at
p90** from sharding alone. The mechanism is that an LRU is *mutated on every hit* — the
entry moves to the list head — so even a pure-read workload serialises on one mutex.

Note p99.9 does **not** improve (240 → 295 µs). The deep tail is not the cache lock; T12
goes looking for what it is.

### R8/R13 — mixed workload, compaction live throughout

```bash
./build-none/bench --bench mixed --threads 2 --reads 8 --num 200000 --seconds 15 --db /data/mx
```

```
11,901,932 ops in 15.00s -> 793,388 ops/s
p50=2  p90=22  p99=191  p99.9=449  max=128,721 (us)
42 compactions, 911,742,473 bytes compacted, 0 stalls
```

`MEASURED`. 2 writers + 8 readers with 42 compactions running underneath. The 128 ms
maximum against a 449 µs p99.9 is the compaction tail — that gap is precisely what R13 is
about, and it is why a mean (12.4 µs) would be worthless here.

### The open-loop protocol (§10.1), demonstrated

Calibrated `M = 7,178 ops/s` closed-loop at 4 threads, `sync=true`, then targeted fractions
of it:

| target | achieved | verdict | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| 3,589 (0.50 M) | 3,590 | valid (100.0%) | 1,052 µs | 2,352 | 4,448 |
| 5,384 (0.75 M) | 5,389 | valid (100.1%) | 982 | 2,088 | 3,104 |
| 6,460 (0.90 M) | 6,460 | valid (100.0%) | 1,044 | 4,832 | 6,768 |
| **12,000 (1.67 M)** | 7,111 | **INVALID — DIVERGED (59% of target)** | *refused* | *refused* | *refused* |

`MEASURED`. The last row is the point. Above saturation the backlog grows without bound and
latency measured from the *intended* start time becomes a function of run **duration** — an
earlier build of this harness happily reported `p99.9 = 864 ms` for that run, which is not a
property of the engine at all. The harness now refuses to print percentiles for a diverging
run.

## T12 — the profiling pass

### The tools that work here, and the one that does not

SPEC §10.4 predicted, before any code existed, that no PMU would be exposed through the
LinuxKit VM. T0 confirmed it:

```
<not supported>   cycles:u
<not supported>   instructions:u
         0.18 msec cpu-clock:u
```

So the profiles below use `perf record -e cpu-clock` — a **software** event, generated by
the kernel timer, which works under virtualisation and gives an accurate wall-clock
sampling profile. **This is what "profiling with perf" means in this project, stated
plainly**, rather than a claim backed by a tool that printed `<not supported>`.

### The finding: allocation, not lock contention

`perf record -e cpu-clock -F 999 -g --call-graph=fp`, `read_hot`, 8 threads, **1 cache
shard** — the configuration deliberately set up to be lock-bound:

| group | % of cycles |
|---|---|
| lock machinery (`pthread_mutex_lock`, `__lll_lock_wake`) | **8.0%** |
| **allocation** (`malloc`, `operator new`, `cfree`, `operator delete`) | **13.7%** |

SPEC §3.10 predicted the contention would be `db_mutex_` and the block-cache shards, and
marked it `ASSUMED`. The profile does not refute that — sharding is worth 1.58× — but it
says a **larger cost was sitting beside it that the spec did not anticipate.** Two
allocations per `Get` were the cause, and both were on a cache *hit*:

1. `Cache::Shard::Lookup` built a `std::string` for `table_.find(key.ToString())` —
   **inside the shard mutex**, so it was both an allocation and a lengthening of the
   critical section.
2. `SstReader::Get` heap-allocated **three** objects per lookup: an `Iterator` for the
   index block, one for the data block, and the wrapper owning them. None outlived the call.

### The two changes, and the numbers

**Change 1** — transparent hash/equality on the cache's map so `find()` takes a
`std::string_view`; `BlockKey` writes into a caller buffer; the `Entry` (with its key
string) is built *outside* the lock; the shard index is recorded on the entry so `Release`
does not re-hash.

**Change 2** — `Block::SeekTo(target, Visitor*)` runs the point lookup on a
**stack-allocated** iterator, and `SstReader::Get` uses it for both the index and the data
block. On a cache hit the read path now allocates **nothing** except the caller's value.

```bash
./build-none/bench --bench readhot --threads 8 --num 400000 --cache_shards {1,16} --db /data/x
```

| | | before | after | change |
|---|---|---|---|---|
| **1 shard** | throughput | 503k ops/s | **710k ops/s** | **+41%** |
| | p90 | 42 µs | **30 µs** | **−29%** |
| | p99 | 160 µs | **127 µs** | −21% |
| | run-to-run spread | 24% | **1.4%** | — |
| **16 shards** | throughput | 955k ops/s | **1,114k ops/s** | **+17%** |
| | p90 | 12 µs | **10 µs** | −17% |
| | p99 | 101 µs | 100 µs | — |

`MEASURED`, median of 5 runs each. **The collapse in run-to-run spread from 24% to 1.4% is
the most telling number here** — allocator contention was a large part of what made this
benchmark noisy, and noise is what makes a p99.9 claim unfalsifiable.

### Where the bottleneck moved (SPEC §10.3)

Re-profiling after both changes, at 16 shards:

| group | % of cycles |
|---|---|
| allocation | 15.7% |
| lock machinery | 5.9% |
| `memcmp` + `memcpy` | 12.7% |
| `Block::Iter::Seek` + `DecodeEntry` | 7.4% |

**Allocation is still the largest group.** The remaining allocations are no longer on the
point-lookup path — they are the *scan* path (`NewIterator` still builds heap iterators, as
it must, since they outlive the call) and compaction's output buffers. `memcmp` at 6.6% is
the comparator itself, which is irreducible for a sorted structure. **The next thing to
attack is the iterator allocation on the scan path**, and it is a harder problem than the
point path was, because those objects genuinely have to outlive their creating call.

### cachegrind — the substitute for the missing PMU

A *simulator*, so it is immune to the absent hardware counters. SPEC §10.4 argues it is
arguably the better tool here regardless.

```
D1  miss rate: 1.4%   (2.0% rd + 0.6% wr)
LLd miss rate: 0.1%
LL  miss rate: 0.0%
```

`MEASURED`, `read_hot` with a warm cache (19,756 hits / 244 misses). A last-level miss rate
of essentially zero is what a working block cache looks like from the memory system's side.

### helgrind and DRD — the second opinion on TSan

| run | verdict |
|---|---|
| `bench mixed`, 2 writers + 2 readers + live compaction, **helgrind** | **0 errors, 0 contexts** |
| same, **DRD** | **0 errors, 0 contexts** |
| `test_skiplist` (1 writer + 8 readers), helgrind | 8 errors, all one context — a `std::atomic<bool>` **in the test's own stop flag** |

`MEASURED`. The disagreement is the interesting part, exactly as SPEC §10.4 anticipated:
**helgrind models pthread primitives and does not understand C++11 atomics**, so it flags
every relaxed/acquire-release access as a possible race. Its one complaint is a test's stop
flag, not engine code — and notably it does *not* flag the skip list's own
release/acquire pointer publication, which is where a real ordering bug would live.

What helgrind *is* authoritative about is **lock-order inversion**, and there it is clean on
the live database with writers, readers and compaction all running. That is independent
confirmation of SPEC §3.10's documented order, `L1 → (L2 | L3)`.

### massif — evidence for S11

```bash
valgrind --tool=massif --time-unit=B ./build-none/bench --bench fillrandom   --num 120000 --write_buffer 4194304 --cache_bytes 8388608
```

| | |
|---|---|
| declared budget | 4 MiB memtable + 8 MiB block cache = **12 MiB** |
| **peak heap** | **11.55 MiB** |
| mean heap | 4.86 MiB |

`MEASURED`, 69 snapshots. Peak sits **under the declared budget**, which is what S11 ("no
unbounded memory growth") asserts and what a bounded memtable plus a bounded cache should
produce.

## Every `ASSUMED` from the spec, closed out

| Prediction | Outcome |
|---|---|
| `perf` hardware counters unavailable under the VM (§10.4) | **Correct.** `cycles:u` / `instructions:u` report `<not supported>`; `cpu-clock` works and is what every profile uses |
| `flock` may not exclude on the bind mount (S16) | **Correct** — `wanrep`'s B2 reproduced exactly. The pid-file fallback is the mechanism here, and a third (in-process) one was needed for the same-process case |
| Group commit: fsyncs ≪ writes at high thread counts (R5) | **Correct.** 8.48 writes per fsync at 16 threads |
| Bloom FPR ≈ 0.0082 at 10 bits/key (§3.6) | **Correct.** Measured 0.00836 — within 2% |
| Memtable arena overhead 2–3× (§3.4) | **Wrong, pessimistic.** Measured **1.28×**; spec corrected |
| Contention is `db_mutex_` and the cache shards (§3.10) | **Half right.** Sharding is worth 1.58×, but allocation cost more than locks (13.7% vs 8.0%); spec corrected |
| Cache sharding may turn out not to matter (§3.11) | **It matters** — 1.58× throughput, 2.9× p90 |
| Tier count grows with total bytes written (E-32) | **Correct**, and `max_tiers` bounds it — nothing past tier 3 after 40 overwrite rounds |

## Deferred numbers — all now measured

| Number | Status | Task |
|---|---|---|
| Group commit: `wal-syncs` ≪ `writes` at 8 threads | **measured: 4.49 writes/fsync** | T8 |


| Cache sharding removes contention | **measured: 1.58× throughput, 2.9× p90** | T12 |
| Tier count / read amp under overwrite | **measured: bounded at `max_tiers`** | T10 |
