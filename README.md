# `lsmeng` — a concurrent LSM-tree storage engine in C++20

A persistent, ordered key–value storage engine built **from scratch**: its own write-ahead
log, memtable, SST format, Bloom filter, tiered compaction, crash-consistent manifest, and
sharded block cache. No RocksDB, no LevelDB, no third-party libraries at all — the only
things linked are libstdc++ and pthreads.

Résumé project #3 of the Pure Storage set (after `dedupe` and `wanrep`).

## Status

| Task | | |
|---|---|---|
| **T0** | container, build gate, `Env`/`FaultEnv`, S13 enforcement, measured environment | done |
| **T1 / T1b** | primitives, CRC32C (3 runtime-selected paths), the comparator, stats | done |
| **T2** | Bloom filter — **measured FPR 0.836% vs 0.82% theory** | done |
| **T3** | skip list + memtable — lock-free readers, TSan clean | done |
| **T4** | write-ahead log — exhaustive torn-tail recovery | done |
| **T5** | SST format, prefix compression, index, footer, `sst_dump` | done |
| **T6** | sharded LRU block cache + byte-bounded table cache | done |
| **T7** | MANIFEST, VersionSet, recovery, two-process exclusion gate | done |
| **T8** | write path — group commit, log rotation, background thread | done |
| **T9** | read path — merging iterator, snapshots, **model test green** | done |
| **T10** | tiered compaction — the four spec-review fixes | done |
| **T11** | histogram, benchmark harness, measured numbers | done |
| **T12** | profiling pass (`perf`, cachegrind, helgrind, DRD, massif) + `RESUME.md` | done |

**22 test binaries, green in all three configurations** (plain, ASan+UBSan, TSan).

**All 13 tasks complete.**

### Headline measurements (`docs/BENCHMARKS.md`)

- **Group commit:** 6.9× throughput from 1→16 threads with `sync=true`, while the fsync
  count *falls* 8.5×. At 1 thread: **99.6% of a raw-append-plus-fsync floor**, with a full
  sorted index.
- **Bloom filters:** 2.4× throughput and **38× fewer data blocks read** for absent keys;
  measured FPR 0.836% against 0.82% theory.
- **Contention removed, twice:** cache sharding (1.58× throughput, p90 29→10 µs), then —
  found by reading a profile — **allocation, which cost more than lock contention**
  (+41% throughput, p90 42→30 µs, run-to-run spread 24%→1.4%).
- **Model test:** 30,000 random ops against a `std::map` reference — 299 compactions, 592
  reopens, 1,183 forward+reverse scans, **zero mismatches**.
- **helgrind, DRD: 0 errors** on a live database. **massif: peak heap 11.55 MiB** against a
  12 MiB declared budget.

### The documents

| | |
|---|---|
| [`docs/RESUME.md`](docs/RESUME.md) | **every résumé phrase mapped to the measurement that earns it** — read this before an interview |
| [`docs/SPEC.md`](docs/SPEC.md) | the design, plus §11: 21 defects found by attacking v1 on purpose |
| [`docs/CHALLENGES.md`](docs/CHALLENGES.md) | the bug journal — C1–C3 design phase, B1–B21 build phase |
| [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) | every number, with its command line |

## Documents

- **[`docs/SPEC.md`](docs/SPEC.md)** — the design. Architecture (§3), required API (§4),
  25 safety requirements (§5), 39 anticipated edge cases (§6), testing (§7), the 13-task
  build order (§9), benchmarks and profiling (§10), and **§11: the review log** — 21 defects
  found by attacking version 1 on purpose, four of which would have silently lost data.
- **[`docs/CHALLENGES.md`](docs/CHALLENGES.md)** — the bug journal. Every problem hit while
  building, with the reasoning and the wrong hypotheses, not just the fix.

## Building

Everything builds and runs in a container, because every correctness claim here is about
`fsync` ordering, atomic `rename`, directory durability, `flock` semantics and `pread`
behaviour — and macOS and Linux disagree about several of them.

```bash
./scripts/dev.sh ./scripts/check.sh
```

That builds and runs every suite in three configurations: plain, ASan+UBSan, and TSan.
