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

## T0 — deferred to their tasks

| Number | Status | Task |
|---|---|---|
| Group commit: `wal-syncs` ≪ `writes` at 8 threads | `ASSUMED` | T8 |
| Memtable arena bytes vs user bytes (~2–3×) | `ASSUMED` | T3 |
| Bloom FPR vs theory (0.0082 at m=10, k=7) | `ASSUMED` | T2 |
| Cache sharding removes contention | `ASSUMED` | T12 |
| Tier count / read amp under overwrite | `ASSUMED` | T11 |
