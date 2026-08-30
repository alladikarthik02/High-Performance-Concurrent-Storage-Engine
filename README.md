# `lsmeng` — a concurrent LSM-tree storage engine in C++20

A persistent, ordered key–value storage engine built **from scratch**: its own write-ahead
log, memtable, SST format, Bloom filter, tiered compaction, crash-consistent manifest, and
sharded block cache. No RocksDB, no LevelDB, no third-party libraries at all — the only
things linked are libstdc++ and pthreads.

Résumé project #3 of the Pure Storage set (after `dedupe` and `wanrep`).

## Status

| | |
|---|---|
| Specification | **complete** — `docs/SPEC.md` (v2, revised after an adversarial review) |
| Implementation | not started — T0 next |

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
