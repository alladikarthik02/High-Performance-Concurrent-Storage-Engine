# `lsmeng` — bug journal & engineering challenges

Every problem hit while building this engine, written down with the **reasoning**, not
just the fix. Small ones are included on purpose — a shell mistake sits next to a
design-invalidating discovery — because the point is that reading this cold should make
each failure legible, and defensible out loud.

**Format per entry:** Symptom → Hypotheses (including the wrong ones) → How I isolated it
→ Root cause → Fix → Generalizes to.

Wrong hypotheses are kept deliberately. Disproving your own first guess is the most
defensible story there is; a journal that only records correct conclusions hides the part
of the work that was actually hard.

**Numbering.** `C*` entries are from the conception phase — before any code existed.
`B*` entries are build-phase: real bugs hit while writing and running the engine.

**Legend:** 🧱 environment/tooling · 🐛 code defect · 🔬 design-invalidating discovery ·
⚔️ concurrency/timing · 💾 durability/crash-consistency · 📐 spec/planning defect ·
🎓 understanding gap

---

## C1 🔬📐 The project was believed to already exist — it was a 66-line wrapper over RocksDB

**Symptom.** Before any work started, the question was whether résumé project #3
("High-Performance Concurrent Storage Engine") was the same thing as an existing
`Key_Value_Store_project_1`. Both descriptions contain the words *LSM-tree*, *write-ahead
log*, *Bloom filters*, and *compaction*. On a keyword match they are the same project.

**Hypotheses.**
- (H1) They are the same project and #3 is already done. — *The tempting one, and wrong.*
  It would have saved weeks of work, which is exactly why it deserved more scrutiny than
  a keyword match.
- (H2) They overlap partially and the KV store needs extending. — Plausible; would have
  meant grafting a hand-written engine underneath an existing gRPC/Raft service.
- (H3) They are different projects that happen to share vocabulary.

**How I isolated it.** Read the actual source rather than the README. Three numbers
settled it:
- `src/storage/Engine.cpp` is **66 lines**, and ~40 of those are two iterator loops.
  `put`/`get`/`del` are one-line pass-throughs to `rocksdb::DB`.
- `include/storage/Filter.hpp` is **12 lines**. The entire "Bloom filter" is
  `NewBloomFilterPolicy(10)`.
- "Tiered compaction" is one call: `opt.OptimizeLevelStyleCompaction()`.

Then the concurrency half of the bullet: `grep` showed `ThreadPool` and `HashRing` are
**never referenced outside their own files** — dead code. There is no locking anywhere in
the storage path, because RocksDB is internally synchronized and there was nothing to
make fine-grained. `benchmark.cpp` is 38 lines, hammers one hardcoded key, `detach()`es
its threads, and reports only ops/sec — **no latency measurement at all**, which is the
one number the résumé's tail-latency claim depends on.

**Root cause.** Not a code defect — a **planning defect, and a vocabulary trap.** The KV
store *uses* an LSM tree; the résumé bullet claims it *builds* one. Those are different
sentences that share every noun. Where the LSM concepts live is the whole question, and
in that project every one of them lives inside a dependency.

**Fix.** Treat them as two separate projects, which is what they are. The KV store is
genuinely substantial — `RaftNode.cpp` alone is 312 lines of NuRaft integration, and its
`Get`/`GetLinearizable`/`GetStale` read modes are a real distributed-systems story — but
that story is *consensus*, not *storage engine internals*. It gets described as what it
is. This project builds the engine, from scratch, which is why §1.1 of the spec makes
"zero third-party libraries" non-negotiable rather than a stylistic preference.

**Generalizes to.** *Shared vocabulary is not shared work.* When a claim and an artifact
use the same nouns, the question to ask is not "does this project involve X?" but "**if
I deleted the dependency, would X still be here?**" For the KV store the answer was no
for every one of the four nouns. That single question is faster than any code review and
it is the one an interviewer will ask, in the form "walk me through your SST block
format."

---

## C2 🧱 Docker daemon not running — caught during planning, not during a build

**Symptom.** `docker version` returned
`failed to connect to the docker API at unix:///Users/karthikalladi/.docker/run/docker.sock
… no such file or directory`, and — worth noting — **exited 0 anyway**.

**Hypotheses.**
- (H1) Docker is not installed. — Ruled out immediately: the CLI itself ran and produced
  a daemon-specific error, and `dedupe/scripts/dev.sh` already carries a workaround for
  Docker Desktop's credential helper, so Desktop has been installed and working here
  before.
- (H2) Docker Desktop is installed but not started. — Correct.

**Root cause.** Desktop was not running, so no daemon was listening on the socket.
Environmental, not a defect.

**Fix.** Start Docker Desktop before T0. Nothing else in the spec phase needs it.

**Generalizes to.** Two things, and the second is the interesting one.
First: check the environment during *planning*, not at the moment you need it — finding
this at the start of T0 would have interrupted a build, finding it now cost nothing.
Second: **`docker version` exited 0 while printing a connection failure.** A script that
gates on `$?` would have sailed straight past this and failed later somewhere confusing.
`scripts/dev.sh` must test for the daemon with something whose exit status actually
reflects reality (`docker info >/dev/null 2>&1`), and this is a reminder that "the
command succeeded" and "the command printed an error" are independent facts on Unix.

---

## C3 🔬📐 Attacking my own spec found four silent-data-loss bugs before a line of code existed

**Symptom.** SPEC v1 read as finished. It had a 20-item safety checklist, a 30-entry
edge-case inventory, and a section explaining why each design choice was correct. It was
also wrong in four places in ways that would have lost or silently corrupted data.

**How I isolated it.** Six independent adversarial reviews, each given one lens
(durability, concurrency, LSM design, scope/API, testability, résumé honesty) and one
instruction: *break it, do not approve it*. Every finding was then handed to a skeptic
told to **refute** it and to default to "already covered" when uncertain.

**36 raised · 23 refuted · 21 real.**

The refutation step is the part worth keeping. A one-sided review produces a pile of
plausible objections and no way to rank them; making a second pass argue the *other* side
is what separated "the document already answers this" from "this is a bug." Of the 23
rejected, most were answered by §6 — which is the argument for writing an edge-case
inventory *before* the code rather than after: **an objection the document already answers
costs nothing to dismiss, and one it does not answer is usually a bug.**

Full detail on all 21 lives in `SPEC.md` §11. The three most instructive are below,
because each failed for a *different reason*.

---

### C3.1 🔴 Wrong rule — the compaction dedup predicate dropped what a snapshot needed

**Symptom (as it would have appeared).** `Get(key, snapshot)` returns `NotFound` for a key
the snapshot must see. No crash, no checksum failure, no counter out of place. It would
have surfaced weeks later as "the model test fails one time in fifty."

**What v1 said.** `drop = (entry.seq <= oldest_snapshot_seq)`.

**Hypotheses I would have had, in order.**
- (H1) The snapshot's sequence number is captured wrong. — *The natural first guess, and
  wrong.* It is where you look because it is the newest code.
- (H2) The merging iterator is emitting versions out of order. — Also wrong, and expensive
  to rule out.
- (H3) The drop rule itself is wrong. — Correct, and the last place you look, because you
  wrote it while reasoning carefully and it *reads* right.

**Root cause.** The predicate asks about the wrong entry. `Put(k,v1)@40`, snapshot@50,
`Put(k,v2)@60`, `Put(k,v3)@100`: the merge emits v3, emits v2 (60 > 50), then evaluates
v1 — `40 <= 50` → dropped. But v1 is precisely the version the snapshot can see. And it is
not a one-off: **the first version at or below the boundary always satisfies the test**, so
the rule dropped the needed version every single time.

The correct statement is about the *previously emitted* version: an entry is unobservable
only if the version that **supersedes** it is itself visible to the oldest live snapshot.

**Fix.** The `last_seq_for_key` formulation in SPEC §3.8.3, S15 rewritten, E-2(d) added
with an explicit test case.

**Generalizes to.** When a rule is about *which of several things survives*, the predicate
almost always concerns a **neighbour**, not the item in hand. Writing `entry.seq` felt
right because `entry` was the variable in scope — the loop's shape suggested the wrong
subject. Worth asking of any filter inside a merge: *whose sequence number am I actually
reasoning about, and is it the one I typed?*

---

### C3.2 🔴 Wrong confidence — "impossible to get subtly wrong" sat next to the subtle wrongness

**Symptom (as it would have appeared).** A deleted key comes back. Minutes later. Only
under compaction.

**What v1 said.** Drop a tombstone when "the output tier is the last tier that exists,"
justified with: *"Cheap to check, impossible to get subtly wrong."*

**Root cause.** The justification only ever reasoned about **deeper** tiers. But tiered
compaction writes its output *into* a tier that already holds up to T−1 older, non-input
files, and the merge never reads them. Drive `k=v1` down to tier 2; later `Delete(k)`; the
tombstone's compaction outputs into tier 2, which *is* the last tier, so the tombstone is
dropped — and the older sibling still sitting in tier 2 resurrects `v1`.

I had also imported an assumption without its precondition: LevelDB's `IsBaseLevelForKey`
shortcut is safe there because leveled compaction **pulls the overlapping output-level
files into its input set**. Tiered compaction does not. The shortcut does not transfer, and
I had copied the conclusion without the argument.

**Fix.** `no_older_data_can_exist(user_key)` — a range check against every non-input file
in the output tier *and* all deeper tiers, using `FileMetaData` ranges already in memory,
so no I/O. S15 rewritten; E-2(e) added.

**Generalizes to.** Two lessons, and the second is the sharper one. First: **the sentence
"impossible to get wrong" is a marker for an unexamined assumption** — I wrote it instead
of checking, and the phrase was doing the work the check should have. Second: when
borrowing a technique from a system you have read about, **the precondition travels with
it**. LevelDB earns that shortcut by construction; I wanted it without paying for it.

---

### C3.3 🔴 Missing rule — I specified one write-ahead log, and there are always two

**Symptom (as it would have appeared).** After `kill -9`, every acknowledged `sync=true`
write since the last memtable switch is gone. Intermittent, because it depends on where in
the flush cycle the kill lands.

**What v1 said.** One `log_number`, one replay rule, "a log may be deleted after its
memtable is flushed."

**Root cause.** Not an incorrect rule — an **absent** one. From the moment
`MakeRoomForWrite` freezes a memtable until that flush has a durable manifest edit, *two*
logs hold acknowledged data: the one feeding the immutable memtable and the one feeding the
new mutable one. v1 had no `prev_log_number`, no multi-file replay, and no liveness
predicate for GC. Recovery would have replayed one file and thrown the other away.

The reason I missed it is worth recording: I specified the write path and the recovery path
in different sittings, and each was internally consistent. **The bug lived in the seam**,
which is exactly where no single section's review would look.

**Fix.** SPEC §3.3.1 — `prev_log_number`, `min_live_log`, ascending multi-file replay, and
a **per-file** torn-tail rule (a truncated earlier log followed by an intact later one is a
*normal* state, not a stopping condition — getting that wrong would have thrown away good
data on purpose). S5 rewritten; E-36 added; crash tests now sweep kill points through that
window specifically.

**Generalizes to.** Ask of every resource: **"how many of these exist at the worst moment,
not the typical one?"** One memtable is the steady state; two is the interesting state. The
same question applied to manifests found §11.7, where an edit appended to a manifest being
rotated away is lost with no crash involved at all. Both bugs are the same shape: a count I
assumed was one.

---

# Build phase

## B1 🧱💾 `NDEBUG` silently deleted the only mechanism enforcing S13

**Symptom.** The very first compile printed, on every command line:

```
-DLSMENG_ASSERTIONS=1 ... -O2 -g -DNDEBUG -std=c++20
```

Both flags. `LSMENG_ASSERTIONS=1` **and** `NDEBUG`.

**Hypotheses.**
- (H1) Harmless — I set `LSMENG_ASSERTIONS` myself, so my assertions are on. — *Wrong, and
  it is the comfortable reading.* My macro expanded to a plain `assert()`, and `NDEBUG` is
  precisely what turns `assert()` into `((void)0)`.
- (H2) Something in my `CMakeLists.txt` sets `NDEBUG`. — No. **CMake sets it**, as part of
  the default flags for `RelWithDebInfo` (and `Release`). I had defaulted
  `CMAKE_BUILD_TYPE` to `RelWithDebInfo` two lines earlier and not connected the two.

**How I isolated it.** Read the compile line rather than the build summary. `-DNDEBUG`
came after my `add_compile_definitions`, and grepping CMake's own defaults confirmed
`CMAKE_CXX_FLAGS_RELWITHDEBINFO` is `-O2 -g -DNDEBUG`.

**Root cause.** SPEC S13 — *`db_mutex_` is never held across I/O* — is enforced by an
assertion at every blocking `Env` entry point, because holding a lock across I/O is
**neither a data race nor a lock-order inversion**: TSan sees nothing, helgrind sees
nothing, and a lock-order tracker is the wrong instrument entirely (SPEC §11.10). That
assertion was the *only* mechanism. Under `NDEBUG` it compiled to nothing — in the exact
configuration we benchmark and ship.

The failure mode is what makes this worth writing down: nothing would have broken. Every
test would still pass. The build would still print `LSMENG_ASSERTIONS=1`. I would have
believed S13 was machine-checked for the entire rest of the project, and the first
evidence otherwise would have been an unexplained p99.9 spike in T12 with no way to
attribute it.

**Fix.** `LSMENG_ASSERT(cond, msg)` in `include/lsmeng/tracked_mutex.h` — its own check,
its own `fprintf` + `abort()`, no dependence on `assert()` and therefore none on `NDEBUG`.

**Generalizes to.** **An assertion that can be compiled out by a flag you did not choose is
not a safety mechanism, it is a comment.** More generally: when a build defines two things
that contradict each other, the one you did not write wins, because it is the one you are
not looking at. Reading the actual compiler command line — not the build summary — is how
you find that class of problem, and it takes ten seconds.

---

## B2 🐛💾 The crash simulator could not model `rename`, and a test asked two questions at once

**Symptom.** Two of twelve `test_env` cases failed on first run, both with an empty
left-hand side where content was expected:

```
FAIL test_env.cc:195: CHECK_EQ(ReadAll(env, p), "DURABLE") -- lhs= rhs=DURABLE
FAIL test_env.cc:242: CHECK_EQ(ReadAll(env, CURRENT), "v1") -- lhs= rhs=v1
```

Identical symptom, **two entirely different causes** — which is why chasing them as one bug
wasted the first ten minutes.

**Failure 1 — the test was wrong, the code was right.**

*Hypothesis:* `SimulateCrash` truncates to zero instead of to the last synced size.
*Wrong.* The file was not truncated; it was **deleted**.

Tracing it: the test created the file with `NewWritableFile`, which records it in
`unsynced_creates`, appended, `Sync()`ed the file — and never `SyncDir`ed. `SimulateCrash`
step 2 removes files whose *directory entry* was never made durable, so the file went away
entirely. That is correct POSIX behaviour: fsyncing a file does not make its name durable.

So the test was asking two questions at once — "are unsynced bytes dropped?" and "are
unsynced creates dropped?" — and got a correct answer to the second while trying to check
the first. Fixed by `SyncDir`ing right after creation, which also matches what the engine
will really do (SPEC §3.2.1 pre-creates and dir-syncs each log before use).

**Failure 2 — the code was wrong, and wrong in an instructive way.**

I modelled "undo an un-durable rename" as: rename the target back to the source name. For
`rename(CURRENT.tmp, CURRENT)` that moves `CURRENT` back to `CURRENT.tmp` — and leaves **no
`CURRENT` at all**, because the original `CURRENT` (holding `v1`) was destroyed by the
rename itself.

*Root cause:* `rename(2)` **atomically replaces** its target. The old target's contents are
gone the instant the rename returns. An undo therefore has to restore two things, not one:
the source name *and* the bytes the rename overwrote.

*Fix:* `FaultEnv::RenameFileImpl` now reads the target's contents **before** calling
rename, stores them with the pending-rename record, and `SimulateCrash` writes them back
after moving the file to its old name.

**Why this one matters more than it looks.** SPEC §3.9 rotates `CURRENT` by
`write .tmp → fsync → rename → fsync(dir)`, and E-7 says a crash there must leave the *old*
`CURRENT` intact. With the broken simulator, a crash produced **no `CURRENT` at all** — and
a later test asserting "the database still opens" would have failed for the wrong reason,
or worse, an assertion phrased as "CURRENT does not contain the new value" would have
**passed while the database was destroyed**. A crash-consistency test is only as truthful
as its crash model, and a model that is wrong in the lenient direction manufactures false
confidence.

**Generalizes to.** Two things.
1. **When one test fails for two reasons, the identical symptom is a coincidence.** `lhs=`
   empty meant "deleted" in one case and "renamed away" in the other. Splitting them before
   theorising was what made both obvious.
2. **A simulator of a destructive operation must model what was destroyed.** I had modelled
   rename as *move*, and it is *move-and-replace*. Any undo of an operation that overwrites
   has to capture the overwritten state first — which is, not coincidentally, exactly what
   the WAL in this project exists to do.

---

## B3 🧱 The CRC32C hardware intrinsic I planned for does not exist on this machine

**Symptom.** `env_facts` reported `arch: aarch64` and
`ARM CRC extension NOT enabled at compile time`.

**Root cause.** The host is Apple Silicon, so the container is **arm64**, not x86-64.
`_mm_crc32_u64` is an SSE4.2 intrinsic and simply does not exist here. The ARMv8-A
equivalent is `__crc32cd`, gated behind the optional CRC32 architectural extension, and
GCC will not emit it without `+crc` on the target.

**Fix (lands in T1).** Three paths, chosen in this order: `__crc32cd` on aarch64 behind a
`__attribute__((target("+crc")))` function and a **runtime** `getauxval(AT_HWCAP) &
HWCAP_CRC32` check; `_mm_crc32_u64` on x86-64 behind a `CPUID` check; and a portable
table-driven fallback that is always compiled and always correct. All three are verified
against the same RFC 3720 test vectors, and the table version is compared against the
hardware one over random inputs — because "the fast path and the slow path disagree" is a
silent corruption bug, not a performance bug.

**Generalizes to.** **A hardware feature check must be a runtime check, not a compile-time
one.** Compiling with `+crc` and calling the instruction unconditionally produces a binary
that runs fine on the build machine and takes SIGILL on a slightly older CPU. Worth
noticing that this is the same shape as B1: an assumption that holds on the machine in
front of me, silently, until it does not.

---

## B4 🔬💾 `flock` does not exclude on the bind mount — the prediction reproduced exactly

**Symptom.** `env_facts` ran a real two-process test — parent takes `flock(LOCK_EX|LOCK_NB)`,
forks, child opens the same path and tries the same lock — and reported:

```
- bind mount   (/work/scratch): NO  -- flock does NOT exclude
- container fs (/data):         yes -- flock excludes correctly
```

**Why it was tested at all.** SPEC S16 refused to assume this, specifically because
`wanrep`'s bug B2 had already observed `flock` succeeding twice on a Docker Desktop bind
mount. The spec review then escalated it (SPEC §11.18): v1 had wired an **unconditional
`unlink` loop** — orphan collection at every `Open` — to a mechanism already known to have
failed here once.

**Root cause.** The bind mount is not a real filesystem; it is a passthrough into the macOS
host. Advisory locks are not carried across that boundary, so `flock` succeeds locally and
guarantees nothing.

**What it confirms, and what changes.** Nothing in the design, because the design already
branched on this answer — which is the point. Concretely:
- S16's pid-file fallback (`O_CREAT|O_EXCL` + pid + `/proc/<pid>` start time) is **the**
  exclusion mechanism here, not a backup. `flock` is retained only as a cheap first check.
- `Options::gc_orphans_on_open` stays **`false`**. Orphans are reported, never deleted, by
  default. An orphan SST wastes space; a wrongly unlinked live SST ends the database.
- Every test and benchmark database lives on `/data` (container-local), where `flock` does
  work. Only source and results are bind-mounted.

**Generalizes to.** **A prediction is only worth making if you write down what you will do
when it comes true.** SPEC v1 predicted this and specified nothing, which is why the review
flagged it as a hole rather than as foresight. The value was not in guessing right; it was
in the design having a branch to take.

---

## B5 🔬💾 The bind mount fsyncs **6× faster** than the container filesystem — which is the alarming direction

**Symptom.**

```
fsync median, bind mount   (/work):  55.0 us
fsync median, container fs (/data): 339.1 us
```

I expected the opposite. The bind mount crosses a virtio/host-filesystem boundary; `/data`
is overlayfs on the VM's own disk. The one doing more work is faster.

**Hypotheses.**
- (H1) `/data` is slow because overlayfs adds a copy-up per write. — Plausible, and it may
  contribute, but copy-up is a first-write cost and this is a steady-state median over 200
  syncs to an already-created file.
- (H2) **The bind mount is not actually flushing anything.** `fdatasync` returns once the
  request crosses into the host passthrough, without waiting for a real barrier on APFS.
  55 µs is far too fast for a durability barrier and about right for a round trip that
  merely acknowledges.

**Why I did not chase this further.** Distinguishing them properly needs a power cut, which
I cannot perform. But the direction of the result is itself the finding, and it points the
same way as SPEC E-14, which already says this chain is **not** a power-loss guarantee.

**Consequence, and it is a real one.** Two rules, now backed by a measurement rather than a
prediction:
1. Every durability test runs on `/data`, never the bind mount — a filesystem that may be
   lying about `fsync` cannot falsify a claim about `fsync` ordering.
2. `BENCHMARKS.md` reports write-path numbers from `/data` and labels the filesystem. A
   throughput figure taken on the bind mount would be measuring the host's willingness to
   acknowledge, not this engine's write path — and it would flatter us by roughly 6×.

**Generalizes to.** **A benchmark result that is better than physics allows is a bug
report about the benchmark.** The instinct on seeing a surprisingly good number should be
to ask what work is not being done, not to write it down. That instinct is the difference
between a résumé number that survives questioning and one that does not.

---

## B6 🧱 `perf` hardware counters are unavailable — predicted in §10.4, confirmed in T0

**Symptom.**

```
<not supported>      cycles:u
<not supported>      instructions:u
         0.18 msec    cpu-clock:u        #  0.415 CPUs utilized
```

**Root cause.** The container runs inside Docker Desktop's LinuxKit VM, which exposes no
PMU. Hardware events cannot be virtualised without host support, so `cycles`,
`instructions`, `cache-misses` and `perf lock` are all out. The **software** `cpu-clock`
event is generated by the kernel timer and works normally.

**Fix.** No change — SPEC §10.4 predicted this and named the substitutes in advance:
`perf record -e cpu-clock` for sampling profiles (which is what finding a lock-wait hotspot
actually needs), and **cachegrind** for cache behaviour, which *simulates* a cache instead
of reading counters and is therefore immune to the missing PMU. Arguably the better tool
here regardless.

**Generalizes to.** The résumé says "profiling with perf and Valgrind." That claim now
means `perf record -e cpu-clock` and a named set of Valgrind tools, with the limitation
written down — rather than a claim backed by a tool that printed `<not supported>`. Being
able to say *why* the counters are missing, unprompted, is worth more in an interview than
having had them.

---

## B7 🐛 UBSan caught an unaligned load in the hardware CRC path — and the test written to catch alignment bugs did not

**Symptom.** Green in `none` and `thread`. In `address`:

```
src/crc32c.cc:58:49: runtime error: load of misaligned address 0x506000000081
  for type 'const uint64_t', which requires 8 byte alignment
  #2 in extend_is_associative_over_split_points  test_crc32c.cc:67
```

**Root cause.** The aarch64 fast path did
`__builtin_aarch64_crc32cx(c, *reinterpret_cast<const uint64_t*>(data))`. Dereferencing a
`uint64_t*` that is not 8-byte aligned is **undefined behaviour**, whatever the hardware
tolerates. ARMv8 and x86-64 both execute the load happily, which is why it passed in two
of three configurations — but the standard permits the compiler to assume the pointer is
aligned, and at higher optimisation levels it can and does vectorise on that assumption.

I had already used `memcpy` for exactly this reason in `Hash64` and in the portable CRC
path, and then reached for a cast in the two functions written last. The correct form
costs nothing: `memcpy` of 8 bytes compiles to the same single `ldr`.

**The part worth keeping: which test found it.**

I wrote `unaligned_starts_produce_the_same_value` *specifically* to catch alignment bugs.
**It passed.** The bug was found by `extend_is_associative_over_split_points`, which is
about something else entirely.

Why: the alignment test compares the hardware result against the portable result, and both
were *correct*. UB that produces the right answer is invisible to a test that checks
answers. The associativity test happened to call `Extend` with a pointer at every offset
into a string literal — including offset 1 — and UBSan flagged the load itself, regardless
of the value it produced.

So the test that found it did not know it was looking, and the test that was looking could
not see it. **The sanitizer found the bug; the test only supplied the input.** That is the
actual division of labour, and it is why the three-configuration gate is not optional
ceremony: `none` and `thread` were both green, and shipping on either would have shipped
UB.

**Fix.** `std::memcpy` into a local in both the aarch64 and x86-64 paths, with a comment
saying why so it does not get "simplified" back later.

**Generalizes to.** Two things.
1. **A test that asserts on values cannot detect undefined behaviour that yields the right
   value.** Only an instrumented build can. Writing a test *about* alignment gives no
   coverage of alignment; running the suite under UBSan does.
2. **Consistency within a file is not consistency across a file.** The safe idiom was
   already present twice in this codebase and I still wrote the unsafe one, because the
   hardware paths *felt* like a different kind of code. When a rule has an exception in
   your head, that is where the bug goes.

---

## B8 🐛 Sharding a counter is correct for a sum and wrong for a maximum

**Symptom.** `test_stats` failed on its first run:

```
FAIL test_stats.cc:31: CHECK_EQ(s.Get(kMaxStallMs), 800u) -- lhs=3600 rhs=800
```

Eight threads each recorded a high-water mark of 0, 100, 200 … 800 ms. The true maximum is
800. The counter reported **3600** — which is 0+100+…+800.

**Root cause.** I sharded the counters sixteen ways to avoid false sharing (SPEC E-24),
which was right, and then wrote a single `Get()` that **sums the shards**, which was right
for every counter except the one it was wrong for. `Add` + sum is correct. `Max` + sum is
not the maximum; it is the sum of per-shard maxima, and it grows with thread count.

**Why this one is worth more than the two lines it took to fix.** The counter in question
is `lsmeng.max-stall-ms` — the write-stall high-water mark, which is **direct evidence for
R13, the tail-latency claim.** The reported number was inflated 4.5× at eight threads, and
it would have inflated *further* with more threads, so a benchmark showing "p99.9 improved
when we reduced thread count" would have been partly an artefact of the measuring
instrument. A wrong number in a benchmark is worse than a missing one: it gets written down
and defended.

It is also silent by construction. Nothing crashes, nothing corrupts, and the value is
plausible — 3600 ms is a believable stall. Only a test that knew the right answer in
advance could catch it.

**Fix.** How a counter is reduced is now **part of its declaration**, not a property of the
reader: a `CounterKind` table (`kSum` / `kMax` / `kGauge`) sits beside the names, with a
`static_assert` that the tables have the same length, and `Get()` dispatches on it. Gauges
write to shard 0 only, so summing still yields the right answer with nothing to reconcile.

**Generalizes to.** **A performance transformation is only transparent for the operations
it distributes over.** Sharding distributes over `+`. It does not distribute over `max`,
and it would not distribute over "last value wins" or over a percentile either — a sharded
histogram must be merged bucket-wise, not concatenated. The general form of the mistake is
optimising a *representation* while leaving the *reduction* implicit; the fix, both here
and generally, is to make the reduction explicit and let the compiler check that every new
counter declares one.

---

## B9 ⚔️ A false positive from the concurrency test's own memory ordering

**Symptom.** The skip-list race test reported **66 torn observations** out of ~600,000
concurrent reads. A torn read in a lock-free structure is the worst possible result: it
means the release/acquire discipline in `skiplist.h` — the thing SPEC calls "the single
most important memory-ordering fact in the project" — was broken.

**Hypotheses.**
- (H1) The `SetNext` release store is in the wrong place, so a node is reachable before its
  key is initialised. — The obvious suspect, and the expensive one to chase.
- (H2) `max_height_` being `relaxed` loses nodes during a height increase. — Plausible; the
  relaxed store is the one deliberate weakening in the file.
- (H3) **The test's own synchronisation is wrong.** — Correct.

**How I isolated it.** The test folded two different invariants into one counter, so a
failure could not be attributed. Splitting them was the whole diagnosis:

```
(a) torn:        a node whose two redundant key halves disagree
(b) from_future: a key numbered higher than anything published
```

After the split: `0 torn, 0 from-future` — because fixing (b) was also fixing the bug.

**Root cause.** The test loaded the `highest` watermark **once, before** starting a
full traversal of a list that a writer was still growing:

```cpp
const uint64_t hi = highest.load(acquire);   // read once...
for (it.SeekToFirst(); it.Valid(); it.Next())  // ...then walk 40,000 nodes
    if ((k & 0xFFFFFFFF) > hi + 1) torn++;     // comparing against a stale bound
```

By the time the traversal reached the tail, the writer had legitimately published
thousands more keys. The "torn" reads were the test comparing a late observation against a
bound captured long before. Worse, the correct order is the opposite of the intuitive one:
the writer does `Insert(n)` **then** `highest.store(n)`, so a reader can correctly see key
`n` while `highest` still reads `n-1`. The bound must be loaded **after** the observation,
not before, and the check is `k <= hi_after + 1`.

**Fix.** Load `highest` after reading each key; keep the two counters separate so the two
failure modes can never be confused again.

**Generalizes to.** Three things, and the last is the one I want to remember.

1. **A test of a lock-free structure needs its own memory-ordering argument**, written down
   as carefully as the structure's. I had reasoned hard about `SetNext`/`Next` and then
   written the harness by feel.
2. **A concurrency test that folds two invariants into one counter cannot be debugged.**
   The split took two minutes and *was* the diagnosis.
3. **A test failure is not evidence about the code until you know which assertion fired.**
   My first instinct was to re-read `skiplist.h`, and the bug was thirty lines away in the
   file I had just written. The prior should run the other way: new test code is more
   likely wrong than code that has been thought about twice.

Verdict on the actual question: **zero torn reads across ~600k concurrent observations, and
TSan clean at 1 writer + 8 readers** in both the skip-list and memtable suites. S6 holds at
this layer.

---

## B10 🎓💾 Truncating a log at a record boundary is a *clean* EOF, not a torn tail

**Symptom.** The exhaustive truncation test — write five records, truncate at every one of
the 87 possible byte offsets, assert recovery yields a clean prefix — failed at exactly
five offsets: 16, 30, 44, 60, 74.

**How I isolated it.** The offsets were the tell. The file header is 16 bytes and each
record is 9 + payload, so 16, 30, 44, 60, 74 are precisely the **record boundaries**. A
failure at every boundary and nowhere else is not a corruption bug; it is a definition
problem.

**Root cause.** My assertion was:

> if fewer records came back than were written, recovery must report a *reason* other than
> clean EOF.

That is wrong, and the reason is worth stating precisely: **a log truncated exactly at a
record boundary is byte-for-byte identical to a log that simply had fewer records written
to it.** There is no torn record, no partial header, nothing to detect — and no reader
could distinguish the two, because there is no difference. Reporting `kEof` is the only
correct answer.

**Fix.** The assertion now computes how many bytes the returned records account for, and
demands a non-EOF reason only when the truncation point falls *strictly inside* a record.

**Why I am keeping a test bug in this journal.** Because the underlying property is one an
interviewer can reasonably probe — *"how does recovery know the log ended cleanly rather
than being cut off?"* — and the honest answer is: **at a record boundary it does not know,
and it does not need to.** Durability is not "detect every truncation"; it is "never
return a record that was not fully written." A `sync=false` tail that vanishes at a
boundary was never acknowledged, so losing it is correct. Writing the assertion too
strongly is what forced me to articulate that, which is the same shape as B9: the test was
wrong because I had not stated the invariant sharply enough to test it.

**Generalizes to.** **"The system must detect X" is often the wrong requirement; "the
system must never claim something false about X" is the right one.** The first is
impossible here and the second is achievable, and conflating them produces tests that
demand behaviour a correct implementation cannot have.

(Also in this run, a two-minute one: an expected string written as
`std::string("P:k3=v\0 3", 10)` when the literal is 9 bytes. Miscounting the length of a
string literal containing an embedded NUL is a small tax for supporting arbitrary bytes in
keys — and a good argument for the harness printing byte counts on a mismatch, which it
now effectively does via the length in the failure message.)

---

## B11 🐛🔬 The index separator off by exactly one key — the bug the spec predicted by name

**Symptom.** `every_key_is_findable_across_every_block_boundary` failed for 499 of 2000
keys, and the pattern was the whole diagnosis:

```
FAIL CHECK_OK(s) -- NotFound [entry=1980]
FAIL CHECK_EQ(value, ...) -- lhs=dddd... rhs=eeee...  [entry=1980]
FAIL ... [entry=1984]   FAIL ... [entry=1988]   FAIL ... [entry=1992]
```

**Every fourth entry**, and the value returned was always the **previous** entry's. With
`block_size = 200` the file held ~4 entries per block, so the failing keys were exactly the
ones **first in their block** — and `Get` was reading the block *before* the one holding
them. Meanwhile a full forward scan returned all 2000 keys correctly, so the file was not
corrupt; only the lookup path was wrong.

**Root cause.** The index entry for block *i* must have a key `S` satisfying

```
    last_key(i)  <=  S  <  first_key(i+1)
```

I wrote the deferred-emission pattern — hold the index entry until the next block starts,
so a short separator can be chosen — and then used `first_key(i+1)` **itself** as the
separator. That satisfies the left inequality and **violates the right one by exactly one
key**. `index_iter->Seek(target)` finds the first index entry with key `>= target`; when
`target == first_key(i+1)` it matches index entry *i*, whose handle points at block *i*.
The key lives in block *i+1*, so `Get` looks in the wrong block and returns NotFound or the
neighbouring value.

**Fix.** Use `last_key(i)` as the separator. Both inequalities then hold by construction
and there is no boundary to get wrong. It costs a slightly larger index than a
shortest-separator scheme; correctness first, and the optimisation is a named next step.

**Why this entry matters more than the fix.** SPEC §3.5 predicted this bug *by name*,
before any code existed:

> *"Getting the `<=` / `<` boundary wrong by one produces a file where exactly the keys on
> a block boundary go missing — a bug that a random test finds and a hand-written test
> usually does not (E-9)."*

And then I made it anyway. Three things follow.

1. **Predicting a bug does not prevent it.** What the prediction bought was the *test*:
   `every_key_is_findable_across_every_block_boundary` exists only because E-9 was written
   down, and it deliberately sets `block_size = 200` so boundaries are dense — roughly one
   per four entries instead of one per hundred. With the default 4 KiB block size the same
   bug would have hidden behind ~99.7% passing lookups.
2. **The failure was invisible to the obvious test.** The round-trip scan passed. Every
   checksum passed. `sst_dump` would have shown a perfectly well-formed file. Only a
   *point lookup for every key* found it — which is precisely the distinction E-9 drew
   between "a random test finds it" and "a hand-written test usually does not."
3. **The deferred-emission pattern is what created the opportunity.** I copied the shape of
   an optimisation (hold the entry so you can pick a short separator) without implementing
   the optimisation, and the half-copied version had a subtly different invariant. Same
   shape as C3.2, where a LevelDB shortcut was borrowed without its precondition:
   **borrowing a structure without its reason is how you inherit its edge cases and none of
   its guarantees.**

**Generalizes to.** When a lookup path and a scan path can disagree, **test them against
each other, not each against your expectations.** A scan that returns everything and a Get
that finds everything are different claims, and only one of them was true here.

---

## B12 🐛💾 Recovery trusted a counter that the file set could contradict — and would have overwritten a live SST

**Symptom.** One assertion in the reopen test:

```
FAIL CHECK_GT(f.vset->NewFileNumber(), 5u) -- lhs=4 rhs=5
```

After recovering a manifest that records a live file numbered **5**, the version set was
prepared to hand out **4**, and then **5**, as *fresh* file numbers.

**Root cause.** The MANIFEST carries `next_file_number` as an explicit field, and
`Recover()` simply believed it: `next_file_number_ = next_file + 1`. But nothing on disk
guarantees that field is consistent with the file numbers actually recorded in the same
manifest. A crash between allocating a number and recording the counter, an older build, or
a caller that constructed a `FileMetaData` without allocating through `NewFileNumber()`
all produce a manifest where the counter is behind the file set.

**Why it is worse than an off-by-one.** The next flush would create `000005.sst` — directly
on top of a **live SST the manifest still references**. Every checksum would pass. The
manifest would be internally consistent. Reads would return whatever the new file happened
to contain, for keys that used to be in the old one. Silent, total loss of one file's data,
with nothing anywhere reporting an error.

**Fix.** Trust the **file set**, not the counter. `Recover()` now recomputes a high-water
mark over every file number, log number and prev-log number it observes, and takes
`max(next_file + 1, highest_seen + 1)`. `LogAndApply` additionally advances the counter past
anything an edit records, so a stale manifest cannot be produced in the first place. Both
sides, because one-sided invariants are what produced this.

**Generalizes to.** **A derived value stored alongside the data it is derived from is a
consistency obligation, not a shortcut.** Every cache of a computed fact — a count, a
maximum, a "next" pointer — can drift from the thing it summarises, and the recovery path
is exactly where you must not assume it has not. Where recomputing is cheap (here: a
max over records already being parsed), recompute. The stored field is then a hint that
can only ever make you *more* conservative, never less.

---

## B13 🔬 Three lock mechanisms, and the gap between the two I had

**Symptom.** The two-process exclusion test, run on **both** filesystems:

```
/data (container fs):       4 tests, 0 failures
/work/scratch (bind mount): 4 tests, 3 failures
```

The *cross-process* case passed everywhere. The failing case was the **same-process** one:
a second `LockFile()` on a path this very process already held returned `OK` on the bind
mount.

**Root cause, and why neither existing mechanism caught it.**

- **`flock`** is per **open file description**, not per process. A second `LockFile()`
  opens a *new* fd, and `flock` on a new description is granted. On the container
  filesystem the kernel still detects the conflict; on the bind mount `flock` does nothing
  at all (T0 measured this; `wanrep` B2 saw it first), so it granted the lock outright.
- **The pid file** could not catch it either, and this is the interesting half: the pid in
  the file was **our own pid**. My code deliberately skips the staleness check when
  `other_pid == getpid()` — because a process must be able to reclaim a lock record it
  left behind after its own earlier crash. Same-process reuse and same-process
  double-locking are *indistinguishable* from the file's contents alone.

So the two mechanisms covered "other process, filesystem implements locks" and "other
process, filesystem does not" — and **nothing covered "this process."**

**Fix.** A third mechanism: a process-wide `std::set<std::string>` of locked paths, checked
**first** (it is the only one correct on every filesystem, and failing there costs no file
descriptor). Each of the three now has a stated job:

| mechanism | covers |
|---|---|
| in-process lock table | this process, every filesystem |
| `flock` | other processes, where advisory locks work |
| pid + start time | other processes, where they do not |

Both filesystems now pass all four cases.

**Generalizes to.** Two things.

1. **Run the environment-dependent test in every environment you actually use.** The bind
   mount is not a hypothetical; the build tree lives on it. Testing only the "good"
   filesystem would have shipped a hole that appears only where the code is developed.
2. **When you add a fallback for a mechanism, enumerate what each one covers.** I had
   reasoned "flock, plus a pid file for where flock fails" and stopped — a two-item list
   that felt exhaustive because it covered the *failure I knew about*. The uncovered case
   was not a failure of either mechanism; it was in the space **between** them, which is
   precisely where a list assembled by patching does not look.

---

## B14 🐛⚔️💾 My own S13 assertion caught a manifest fsync under `db_mutex_` — and a second bug deleted the live manifest

**Symptom.** Two failures in one run of the new write-path suite.

```
LSMENG ASSERT FAILED include/lsmeng/env.h:60
  SPEC S13 violated: db_mutex_ held across a blocking Env call
  #7 lsmeng::VersionSet::LogAndApply     version_set.cc:251
  #8 lsmeng::DBImpl::FlushImmutableMemtable  db_impl.cc:341

FAIL REQUIRE_OK(d.Open()) -- Corruption: CURRENT names manifest
     MANIFEST-000002 which cannot be read: NotFound
```

**Bug 1 — the assertion did its job.** `FlushImmutableMemtable` held `db_mutex_` across
`LogAndApply`, which appends a `VersionEdit` to the MANIFEST **and fsyncs it**. A manifest
fsync is milliseconds (T0 measured 339 µs median on `/data`, and that is the fast case);
holding the lock across it blocks every queued writer and every `Get`, which is precisely
the p99.9 damage R13 exists to remove. It is also invisible: no wrong answers, no crash,
just a mysterious latency spike weeks later.

This is **exactly the failure mode SPEC §11.10 was written about** — v1's `MakeRoomForWrite`
created a log file under the mutex, the review caught it on paper, and the response was to
build a machine that would catch it in code. Then I reintroduced the same class of bug in a
different function, and the machine caught it in under a second. `TrackedMutex` +
`LSMENG_ASSERT` paid for itself the first time the write path existed.

*Fix.* `LogAndApply` now takes the caller's mutex and **releases it across the append and
the fsync**, re-acquiring before the in-memory install. That is only safe because exactly
one thread is ever inside it (SPEC §3.8.4's single background thread), so I asserted that
too rather than assuming it: a `std::atomic<bool> appending_` exchange that fires if a
second thread ever arrives (S23, E-38).

**Bug 2 — `RemoveObsoleteFiles` deleted the manifest that `CURRENT` was pointing at.**

`Recover()` set `manifest_file_number_ = next_file` — the *next file number to allocate*,
not the number of the manifest actually in use. GC keeps manifests with
`number >= manifest_number`, so once the file counter had moved past the live manifest's
number, the live manifest was classified as obsolete and unlinked. `CURRENT` then named a
file that no longer existed: **an unopenable database**, which is the exact failure §3.9
calls "unrecoverable rather than merely untidy."

It survived one reopen and died on the second, which is why a single round-trip test would
have missed it.

*Fix.* Parse the number out of `CURRENT`'s contents and track it separately. Two variables
that happened to be equal at the moment I wrote the code are not one variable.

**Generalizes to.** **A quantity's name has to say which quantity it is.** `next_file` and
`manifest_file_number_` were both "a file number around the manifest," and conflating them
read as harmless. The same mistake in B12 was `next_file_number` versus the actual maximum
file number in the set. Twice in one task, both around file numbering — which suggests the
real lesson is that a counter shared by several concerns wants one accessor per concern,
not one field per family.

---

## B15 🐛 Group commit looked broken because the counter read a batch it had already cleared

**Symptom.**

```
MEASURED 362 writes, 719 fsyncs at 8 threads -> 0.50 writes per fsync
FAIL CHECK_EQ(writes, 3200) -- lhs=362
FAIL CHECK_LT(syncs, writes) -- lhs=719 rhs=362
```

More fsyncs than writes, and only 11% of the writes accounted for. On its face: group
commit inverted, each write paying for *two* fsyncs.

**Root cause.** The instrument, not the engine.

```cpp
if (batch == tmp_batch_) tmp_batch_->Clear();   // <-- clears the merged group buffer
if (status.ok()) stats_.Add(kWrites, batch->Count());   // <-- Count() is now 0
```

`tmp_batch_` is the buffer a leader merges the group into. When a leader batched *any*
followers, `batch` pointed at `tmp_batch_`, which was cleared one line before its count was
read — so the write counted **zero**. Only writes that were *not* grouped (a leader with no
followers, where `batch` is the caller's own) counted at all.

**The part that makes this worth writing down.** The bug is **anti-correlated with the
thing it measures.** The better group commit worked, the more writers each leader absorbed,
and the fewer writes got counted. A perfectly functioning batch of 8 recorded 0; a
completely unbatched write recorded 1. So the metric that exists to *prove* R5 reported
almost exactly the opposite of the truth, and it reported it as a plausible number rather
than an obvious one — 0.50 writes per fsync looks like a real, bad result, not like a bug.

With the count read before the clear: **3200 writes, 720 fsyncs, 4.44 writes per fsync.**

**Generalizes to.** Two things.
1. **Read a value out of a buffer before you recycle the buffer** — obvious in isolation,
   invisible when the recycle line was written first and the counter added later.
2. **When a measurement contradicts the mechanism you believe you implemented, suspect the
   measurement first.** Same family as B8 (a max reduced by summing) and B9 (a race test
   with its own broken ordering). Three of the sixteen bugs in this journal so far were in
   the instruments rather than the engine, which is roughly what you would expect and
   exactly why "the number looked wrong so I changed the code" is a dangerous instinct.

Also fixed here: the test ignored every `Put`'s `Status`. A test that counts things must
never discard the return value of the thing it is counting.

---

## B16 🐛 Every flushed memtable leaked its whole arena, because the constructor already granted a reference

**Symptom.** Green in `none` and `thread`; in `address`:

```
==11==ERROR: LeakSanitizer: detected memory leaks
Direct leak of 352 byte(s) in 4 objects allocated from:
    #1 lsmeng::DBImpl::SwitchMemtableLocked()  db_impl.cc:303
```

**Root cause.** `MemTable`'s constructor initialises `refs_{1}` — the creator's reference.
I then wrote `mem_ = new MemTable(); mem_->Ref();`, so every memtable started at **2**. On
flush, `imm_->Unref()` took it to 1 and it was never destroyed. Every memtable that was ever
flushed leaked its entire arena — bounded by `write_buffer_size` each, so a long-running
database would leak megabytes per flush, indefinitely.

**Why only ASan found it.** Nothing breaks. Every read returns the right answer, every write
succeeds, every crash test passes. The only symptom is memory that never comes back, which
no assertion in the suite is watching for. This is the third time in this project that the
sanitizer, not the test, was the thing that found the bug (B7 unaligned load, B14's
assertion, this) — the tests supplied the workload and the instrumented build supplied the
verdict.

**Fix.** Delete the `Ref()` at every construction site, and say so in `memtable.h` where the
next person will look: *the constructor grants one reference; `Ref()` is for **additional**
holders — a reader pinning it, or a flush pinning it while it works.*

**Generalizes to.** **A refcounted type must document what its constructor's count means, at
the constructor.** "Starts at 1" and "starts at 0, caller must Ref" are both defensible
designs, and the caller cannot tell them apart from the header. Ownership conventions that
live only in the author's head become leaks the moment a second call site appears.

---

## B17 🐛 Two bugs in one iterator: an infinite loop, and I/O under the lock again

**Symptom 1 — the scan never ended.** The first read test printed its name and hung.

*Root cause.* `DBIter::Next()` must skip every remaining version of the current user key
before looking for the next one. It compared against `key_`, which stores the **internal**
key (user key + 8 tag bytes), so the comparison against real user keys never matched. The
skip loop ran zero times, `FindNextVisible()` re-found the entry it was already on, and the
caller's `for (...; it->Next())` spun forever.

The interesting part is that this is not a subtle logic error — it is a **units error**.
`key_` and "the current user key" are both `std::string`s holding key-shaped bytes, and the
type system cannot tell them apart. The same confusion appears twice more in this file, and
in both other places I had written `ExtractUserKey(...)` without thinking about it. The one
that broke was the one where the variable was already sitting there looking correct.

**Symptom 2 — the S13 assertion fired again**, at `Version::AddIterators`.

`NewIterator` took `db_mutex_`, pinned the memtable and the Version, and then called
`AddIterators` **inside the same critical section** — and `AddIterators` opens every live
SST through the table cache. That is a file open per file, under the lock that every writer
and every `Get` needs.

*Fix.* Pin the Version under the lock, then build the iterators **outside** it. This is safe
for precisely the reason S14 exists: the Version is immutable and refcounted, so once pinned
it cannot change or be freed, and nothing about it needs the mutex any more.

**Generalizes to.** That is the **second** time (after B14) that the S13 assertion caught
I/O under `db_mutex_` in code I had just written while thinking about S13. Both times the
pattern was identical: a function that *looks* like bookkeeping (`LogAndApply`,
`AddIterators`) turning out to do I/O one or two calls down. The lesson is not "be more
careful" — it is that **"does this do I/O?" is not answerable by looking at a call site**,
and a machine that answers it at runtime is worth more than any amount of care. This is the
concrete argument for R9's claim being *enforced* rather than *asserted in prose*.

---

## B18 🐛 A use-after-free from violating my own iterator contract

**Symptom.** Green in `none` and `thread`. Under ASan:

```
ERROR: AddressSanitizer: heap-use-after-free
    #1 lsmeng::Slice::ToString() const
    #2 SeekToLast   src/db_iter.cc:69
```

**Root cause.** `SeekToLast` did:

```cpp
ParsedInternalKey p;
ParseInternalKey(iter_->key(), &p);        // p.user_key points INTO a data block
if (ResolveUserKey(p.user_key.ToString())) return;   // ...which re-seeks...
PrevFrom(p.user_key.ToString());           // ...and this reads freed memory
```

`ResolveUserKey` performs a `Seek`, which makes the SST's two-level iterator drop its
current data block and load a different one. The block's buffer is freed, and `p.user_key`
— a `Slice`, i.e. a bare pointer and length — is left dangling. The first `.ToString()` is
evaluated before the call and is fine; the second one, after, is not.

**What makes it worth a full entry.** `iterator.h` states the contract in its own header
comment, which I wrote:

> *`key()` and `value()` are only valid while `Valid()`, and only until the next movement.
> They point INTO the underlying block or arena; they are not copies.*

That non-owning design is deliberate and is what makes a scan allocation-free. But it means
**every** local `Slice` derived from an iterator is invalidated by **any** movement of that
iterator, including movements that happen several frames deeper inside a helper. Three other
places in the same file get this right (`FindNextVisible`, `PrevFrom`, `Next` all copy
first); the one that got it wrong is the one where the copy looked redundant because
`.ToString()` was already being written on the line.

**Fix.** Copy the user key into a `std::string` once, before anything that can move the
iterator, and use that.

**Generalizes to.** **A non-owning view is a lifetime obligation that the type system does
not track, and the danger scales with how far away the invalidating call is.** `Slice`,
`string_view`, and every span-like type share this. The practical rule that would have
prevented it: *if a view is used after any call that is not obviously pure, copy it at the
point of creation* — the copy is cheap, and the alternative is a bug that three of four call
sites avoid by luck rather than by rule. Also, again: **only the sanitizer could see this.**
Four of the bugs in this journal (B7, B14, B16, B18) were invisible to every assertion in
the suite and visible immediately to an instrumented build.

---
