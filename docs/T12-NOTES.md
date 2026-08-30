# T12 — profiling pass, in progress

Where the work stopped, so it can be resumed without re-deriving anything.

## What has been done

`perf record -e cpu-clock` works (T0 confirmed hardware counters are unavailable under the
LinuxKit VM, exactly as SPEC §10.4 predicted; the software event is the substitute named
there). A first profile is captured:

```bash
PERF=$(ls /usr/lib/linux-tools-*/perf | head -1)
$PERF record -q -e cpu-clock -F 999 -g --call-graph=fp -o /data/prof/one.data -- \
  ./build-none/bench --bench readhot --threads 8 --num 300000 --cache_shards 1 --db /data/prof/db1
$PERF report -i /data/prof/one.data --stdio --no-children --percent-limit 1.5
```

## The profile (read_hot, 8 threads, **1** cache shard — the deliberately contended case)

```
 8.12%  memcmp
 6.21%  __memcpy_generic
 5.41%  pthread_mutex_lock          <-- lock machinery
 4.78%  lsmeng::Block::Iter::Seek
 4.30%  malloc                      <-- allocation
 3.82%  lsmeng::crc32c::ExtendArm
 3.50%  operator new                <-- allocation
 3.34%  lsmeng::SkipList::FindGreaterOrEqual
 3.34%  cfree                       <-- allocation
 2.87%  lsmeng::BlockBuilder::Add
 2.55%  __GI___lll_lock_wake        <-- lock machinery
 2.55%  operator delete             <-- allocation
 2.39%  __kernel_clock_gettime
 2.07%  lsmeng::DecodeEntry
```

## The finding, and why it is the interesting one

Grouping the rows:

- **lock machinery ≈ 8.0%** (`pthread_mutex_lock` + `__lll_lock_wake`)
- **allocation ≈ 13.7%** (`malloc` + `operator new` + `cfree` + `operator delete`)

**Allocation costs more than lock contention, in the configuration deliberately set up to
be lock-bound.** SPEC §3.10 predicted the contention would be the block cache and
`db_mutex_`, and marked that `ASSUMED`. The profile does not refute it — the T11 sharding
sweep already showed 1→16 shards is worth 1.58× throughput and 2.9× at p90 — but it says
there is a **larger** cost sitting next to it that the spec did not anticipate.

### Where the allocations come from (read by inspection, not yet confirmed by a callee profile)

A single `Get` on a cache **hit** currently allocates at least:

1. `Cache::BlockKey(file_number, offset)` → returns a `std::string` (16 bytes, heap).
2. `Cache::Shard::Lookup` → `table_.find(key.ToString())` — **another `std::string`, and it
   is constructed while holding the shard mutex.** So the allocation is both a cost in
   itself and a lengthening of the critical section, which is why it shows up beside the
   lock rows rather than instead of them.
3. `Block::NewIterator()` → `new Iter`, twice (index block and data block), each carrying a
   `std::string key_` that grows during `ParseNextKey`.
4. `new BlockOwningIterator` to own the cache handle.

### The intended fix (R12's named change)

- Make `BlockKey` write into a caller-supplied `char[16]` and pass a `Slice`.
- Give the cache's `unordered_map` a **transparent** hash/equality (`is_transparent`) so
  `find()` accepts a `std::string_view` and allocates nothing — this also shortens the
  critical section, since the temporary was being built under the lock.

Then re-run the same command and report before/after throughput and p50/p90/p99/p99.9, plus
**where the bottleneck moved**, which is what SPEC §10.3 asks for.

## Still to do in T12

- [ ] Apply the allocation fix, measure before/after at a fixed configuration.
- [ ] cachegrind (a simulator, immune to the missing PMU — SPEC §10.4 argues it is the
      better tool here anyway).
- [ ] helgrind + DRD as a second opinion to TSan on the stress suite.
- [ ] massif against `write_buffer_size`, as evidence for S11.
- [ ] `docs/RESUME.md`: every résumé phrase mapped to the measurement that earns it.
