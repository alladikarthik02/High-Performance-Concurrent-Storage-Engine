#pragma once
#include <cstddef>

namespace lsmeng {

class Env;
class Snapshot;

// SPEC 4.1. Defaults are the ones the benchmarks in SPEC 10.2 sweep around.
struct Options {
  // nullptr means Env::Default(). Tests pass a FaultEnv here -- SPEC 11.11: v1 had no way
  // to inject one, which made the entire fault-injection strategy unreachable.
  Env* env = nullptr;

  size_t write_buffer_size = 4 << 20;      // memtable arena bytes before freeze
  size_t block_size = 4 << 10;
  int block_restart_interval = 16;
  int bloom_bits_per_key = 10;             // 0 disables filters entirely
  size_t block_cache_bytes = 8 << 20;
  size_t filter_memory_bytes = 16 << 20;   // resident Bloom filters (S11, E-34)
  int cache_shards = 16;                   // 1 = unsharded, for the T12 experiment
  int tier_trigger = 4;                    // T
  int max_tiers = 7;                       // SPEC 3.8.2 -- bounds tier growth (E-32)
  int max_open_files = 500;
  bool paranoid_checks = true;
  bool create_if_missing = true;
  bool error_if_exists = false;
  // SPEC 3.9 / E-6: deleting files is OPT-IN. flock does not exclude on this container's
  // bind mount (CHALLENGES B4), and an orphan SST only wastes space while a wrongly
  // unlinked live SST ends the database.
  bool gc_orphans_on_open = false;
};

struct ReadOptions {
  const Snapshot* snapshot = nullptr;
  bool fill_cache = true;
  bool verify_checksums = true;
};

struct WriteOptions {
  bool sync = false;
};

// SPEC 3.1. Enforced at every public entry point (S3): never truncated, never accepted.
constexpr size_t kMaxKeySize = 4 << 10;
constexpr size_t kMaxValueSize = 1 << 20;
constexpr size_t kMaxBatchSize = 8 << 20;

}  // namespace lsmeng
