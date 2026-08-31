# `lsmeng` — final walk of the SPEC §5 safety checklist

SPEC §5 says: *"Walk this list top to bottom at the end of each task. Every item is either
still true, or the task is not finished."* This is the final walk, at the end of T12.

⚙️ = enforced by a machine (an assertion, a sanitizer, or a script), not by reading.

| # | Requirement | Status | What establishes it |
|---|---|---|---|
| S1 | `sync=true` write is in the WAL and fsynced **before** the ack and before the memtable insert is visible | ✅ | Write path does WAL → `Sync()` → memtable insert, in that order (`db_impl.cc` step 6). `acknowledged_sync_writes_survive_a_simulated_crash`: **0 of 300 lost** through `FaultEnv`, which drops un-fsynced bytes — the assertion a `kill -9` test cannot make |
| S2 | A `sync=true` writer is never committed by a group whose leader does not fsync | ✅ | `BuildBatchGroup` breaks at the first differing `sync` flag. `a_sync_writer_is_never_committed_by_a_group_that_does_not_fsync` runs 8 threads with alternating flags |
| S3 | Every entry point validates sizes and rejects with `InvalidArgument` — never truncates | ✅ | `ValidateKeyValue` + the batch cap. `oversized_input_is_rejected_never_truncated` tests at, and one past, each limit |
| S4 | Every on-disk record CRC32C-verified before use; `len == 0` invalid; WAL/MANIFEST CRCs seeded with the file number | ✅ | `wal.cc` four checks. Tested by truncating at **all 87 byte offsets** and by flipping a bit at **every byte position** |
| S5 | Durability ordering never inverted; a log is unlinked only after its flush has a durable manifest edit and `N < min_live_log` | ✅ | `WriteLevel0Table` → `LogAndApply` → install → `RemoveObsoleteFiles`. `prev_log_number`/`MinLiveLog` in `version_set.h`; E-36 test replays both live logs |
| S6 | ⚙️ TSan clean with ≥4 writers, ≥4 readers, live flush and live compaction | ✅ | `check.sh` thread configuration: **22/22 green**. `compaction_runs_concurrently_with_readers_and_writers`: 86,755 reads, 0 inconsistent |
| S7 | No block/index/filter used without CRC verification under `paranoid_checks` | ✅ | `SstReader::ReadBlock`. `a_corrupt_block_is_caught_by_its_checksum` |
| S8 | Missing or corrupt Bloom filter degrades to "maybe present", never "not present" | ✅ | `BloomFilter::MayContain` returns true for every degenerate input; `SstReader::Open` clears the filter rather than failing. `missing_or_corrupt_filter_fails_open_never_closed` |
| S9 | Sequence numbers unique, monotonic, from 1, and survive reopen | ✅ | Recovered from the manifest and advanced past WAL replay. `last_sequence_survives_reopen_and_never_restarts_at_zero`, `data_survives_reopen_and_sequence_numbers_do_not_restart` |
| S10 | Exactly one comparator, a strict weak ordering, used by all four consumers | ✅ | `dbformat.h`, single definition. `comparator_is_a_strict_weak_ordering_by_brute_force` over 108 adversarial keys |
| S11 | No unbounded growth: memtable, block cache, **resident filters**, writer queue | ✅ | **massif peak heap 11.55 MiB against a 12 MiB declared budget.** `resident_filter_bytes_are_bounded_not_just_the_file_count` shows the byte limit binding where the count limit does not (E-34) |
| S12 | ⚙️ Exactly one thread writes a given memtable | ✅ | Group-commit head-of-queue invariant; only the leader inserts. TSan clean at 4 writers |
| S13 | ⚙️ `db_mutex_` **never** held across I/O — no exceptions | ✅ | `TrackedMutex` + `LSMENG_ASSERT` at every blocking `Env` entry point. **Caught two real violations** (B14 manifest fsync, B17 `AddIterators`); silent in all 22 suites since |
| S14 | A file is unlinked only when no live `Version` references it | ✅ | `AddLiveFiles` over the version list + `pending_outputs_`. `live_files_are_the_union_over_all_live_versions` |
| S15 | Non-newest version dropped only if the **previously emitted** version ≤ oldest snapshot; tombstone additionally needs `no_older_data_can_exist` | ✅ | `compaction.cc` merge loop. `e2d_…` and `e2e_…` reconstruct the two v1 bugs directly |
| S16 | One process per directory — flock + pid/start-time + **in-process table** | ✅ | Three mechanisms, one per failure mode (B13). Two-process test green on **both** filesystems |
| S17 | Every error path returns a `Status`; I/O errors are sticky | ✅ | `bg_error_` set and returned by every later `Write`; no `exit`/`abort` on a recoverable condition |
| S18 | ⚙️ Randomised tests print their seed and honour `LSMENG_SEED` | ✅ | `testing::seed()`, printed in every suite header |
| S19 | ⚙️ No leaks under LeakSanitizer, including error paths | ✅ | ASan configuration green. **Found B16** (memtable arena) and **B19** (VersionBuilder on recovery) |
| S20 | ⚙️ ASan+UBSan and TSan clean on every suite | ✅ | `check.sh`: **22/22 in all three configurations** |
| S21 | A read's sequence number and its refs acquired in **one** critical section | ✅ | `DBImpl::Get` and `NewIterator` both. This was SPEC v1's stale-read bug (§11.5) |
| S22 | ⚙️ Every `bg_cv_` wait has an escape predicate for `bg_error_` and `shutting_down_`; every setter broadcasts | ✅ | All four wait sites. Plus a watchdog that converts a permanent stall into a *named* error rather than a hang |
| S23 | ⚙️ MANIFEST appends totally ordered, equal to install order; one appender | ✅ | Single background thread + `appending_.exchange` assertion that fires on a second appender |
| S24 | ⚙️ No engine I/O except through `Options::env` | ✅ | `check.sh` runs the grep **as a test**, before the builds. Clean |
| S25 | No test branches on `Status::ToString()` | ✅ | `CHECK_CODE` / `Is*()` throughout; the model test's miss/corruption distinction depends on it |

**25 of 25 hold. 12 are machine-enforced.**

## The three that changed shape during the build

- **S13** began as "enforced by inspection" in SPEC v1. The review pointed out that
  inspection cannot see this class of bug, and the assertion that replaced it then caught
  two real violations in code written *while thinking about the rule*.
- **S16** began as `flock` alone. It took **three** mechanisms — one per failure mode —
  and the third was only discovered by running the two-process test on the bind mount as
  well as the container filesystem (B13).
- **S11** began bounding open files by **count**. T2's measurement (1.25 bytes/key of
  resident filter) showed a count bound is not a memory bound at all, so a byte bound was
  added (E-34).
