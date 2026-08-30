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
cpu-clock` plus cachegrind (a *simulator*, immune to the missing PMU) — and R11's résumé
phrase is backed by a tool that actually produced output, not one that printed
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

## T0 — deferred to their tasks

| Number | Status | Task |
|---|---|---|
| Group commit: `wal-syncs` ≪ `writes` at 8 threads | `ASSUMED` | T8 |


| Cache sharding removes contention | `ASSUMED` | T12 |
| Tier count / read amp under overwrite | `ASSUMED` | T11 |
