// SPEC 10. The benchmark harness.
//
// EVERY RUN REPORTS PERCENTILES, NOT A MEAN (10.1). R13 claims a p99.9 improvement, and a
// mean cannot express that: 999 operations at 10 us plus one at 200 ms averages 210 us,
// which describes neither population.
//
// OPEN LOOP IS A PROTOCOL, NOT A FLAG (10.1). A closed-loop benchmark cannot observe a
// stall correctly -- while the engine is stuck the loop simply issues fewer requests, so
// the histogram under-reports exactly the tail R13 is about. Open loop measures from each
// operation's INTENDED start time. But open loop is only meaningful below saturation, and
// SPEC 3.8.4's stop trigger guarantees saturation is reachable, so a run whose backlog
// diverges is reported as "diverged at rate R" and NEVER as a percentile.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "lsmeng/db.h"
#include "lsmeng/env.h"
#include "lsmeng/histogram.h"
#include "lsmeng/table_cache.h"

using namespace lsmeng;
using Clock = std::chrono::steady_clock;

namespace {

struct Config {
  std::string bench = "fillrandom";
  std::string dbdir = "/data/bench";
  int threads = 1;
  int num = 200000;
  int key_size = 16;
  int value_size = 100;
  bool sync = false;
  int bloom_bits = 10;
  int cache_shards = 16;
  size_t cache_bytes = 8 << 20;
  size_t write_buffer = 4 << 20;
  int tier_trigger = 4;
  int max_tiers = 7;
  int reads = 0;                 // for mixed
  double open_loop_rate = 0;     // ops/sec; 0 = closed loop
  int seconds = 0;               // for open loop / mixed
  std::string label;
};

std::string MakeKey(const Config& c, uint64_t n) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "key%0*llu", c.key_size - 3, (unsigned long long)n);
  return std::string(buf, static_cast<size_t>(c.key_size));
}

// Zipfian-ish: skew accesses toward a small hot set. Not a rigorous Zipf generator -- what
// `read_hot` needs is a distribution where a minority of keys take the majority of traffic,
// so cache and lock behaviour are exercised rather than pure random misses. Saying that is
// better than calling it Zipf and being asked for the exponent.
uint64_t HotKey(uint64_t r, uint64_t n) {
  const uint64_t hot = n / 100 + 1;   // 1% of the key space
  return (r % 100 < 90) ? (r % hot) : (r % n);
}

struct ThreadResult {
  Histogram hist;
  uint64_t ops = 0;
  uint64_t found = 0;
  uint64_t divergence = 0;   // open loop: ops whose intended start had already passed badly
};

uint64_t NowMicros() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
          .count());
}

void PrintHeader(const Config& c, const char* mode) {
  std::printf("\n=== %s%s  [%s] ===\n", c.bench.c_str(),
              c.label.empty() ? "" : (" " + c.label).c_str(), mode);
  std::printf("    threads=%d num=%d key=%dB value=%dB sync=%s bloom_bits=%d "
              "cache_shards=%d cache=%zuKiB write_buffer=%zuKiB T=%d\n",
              c.threads, c.num, c.key_size, c.value_size, c.sync ? "true" : "false",
              c.bloom_bits, c.cache_shards, c.cache_bytes / 1024, c.write_buffer / 1024,
              c.tier_trigger);
}

void Report(const Config& c, const std::vector<ThreadResult>& results, double elapsed_s,
            DB* db) {
  Histogram all;
  uint64_t ops = 0, found = 0, diverged = 0;
  for (const auto& r : results) { all.Merge(r.hist); ops += r.ops; found += r.found; diverged += r.divergence; }
  const double achieved = ops / elapsed_s;

  // SPEC 10.1, VALIDITY CRITERION. An open-loop run whose achieved throughput falls below
  // 99% of its target is measuring a growing backlog, not the engine: latency from the
  // intended start time then rises linearly with elapsed time, so the reported p99.9 is a
  // function of run DURATION. Such a run is reported as "diverged at rate R" and its
  // percentiles are REFUSED -- printing them would be publishing a number that means
  // nothing, which is exactly the failure this protocol exists to prevent.
  if (c.open_loop_rate > 0 && achieved < 0.99 * c.open_loop_rate) {
    std::printf("    *** INVALID -- DIVERGED at target %.0f ops/s (achieved %.0f, %.0f%% of "
                "target). Percentiles are NOT reported for a diverging run: they would be a\n"
                "        function of run duration, not of the engine (SPEC 10.1 validity "
                "criterion). %llu ops were issued more than 1s behind schedule.\n",
                c.open_loop_rate, achieved, 100.0 * achieved / c.open_loop_rate,
                (unsigned long long)diverged);
    return;
  }

  std::printf("    %llu ops in %.2fs -> %.0f ops/s   %s\n", (unsigned long long)ops,
              elapsed_s, achieved, all.Report().c_str());
  if (c.open_loop_rate > 0)
    std::printf("    valid: achieved %.1f%% of the %.0f ops/s target\n",
                100.0 * achieved / c.open_loop_rate, c.open_loop_rate);
  if (found) std::printf("    found: %llu / %llu\n", (unsigned long long)found, (unsigned long long)ops);
  if (diverged)
    std::printf("    note: %llu ops issued more than 1s late\n", (unsigned long long)diverged);

  static const char* kProps[] = {
      "lsmeng.writes", "lsmeng.wal-syncs", "lsmeng.blocks-read", "lsmeng.filter-blocks-read",
      "lsmeng.bloom-checked", "lsmeng.bloom-rejected", "lsmeng.cache-hits",
      "lsmeng.cache-misses", "lsmeng.compactions", "lsmeng.bytes-compacted",
      "lsmeng.stalls", "lsmeng.max-stall-ms", "lsmeng.live-bytes", "lsmeng.live-entries"};
  std::string line = "   ";
  for (const char* p : kProps) {
    std::string v;
    if (db->GetProperty(Slice(p), &v)) line += " " + std::string(p + 7) + "=" + v;
  }
  std::printf("%s\n", line.c_str());
  for (int t = 0; t < 8; ++t) {
    std::string v;
    if (db->GetProperty(Slice("lsmeng.num-files-at-tier" + std::to_string(t)), &v) && v != "0")
      std::printf("     tier%d=%s", t, v.c_str());
  }
  std::printf("\n");
}

Options MakeOptions(const Config& c) {
  Options o;
  o.write_buffer_size = c.write_buffer;
  o.bloom_bits_per_key = c.bloom_bits;
  o.cache_shards = c.cache_shards;
  o.block_cache_bytes = c.cache_bytes;
  o.tier_trigger = c.tier_trigger;
  o.max_tiers = c.max_tiers;
  o.create_if_missing = true;
  return o;
}

void RunWrite(const Config& c, DB* db, bool sequential) {
  std::vector<ThreadResult> results(static_cast<size_t>(c.threads));
  const std::string value(static_cast<size_t>(c.value_size), 'v');
  const auto start = Clock::now();

  std::vector<std::thread> ts;
  for (int t = 0; t < c.threads; ++t) {
    ts.emplace_back([&, t] {
      ThreadResult& r = results[static_cast<size_t>(t)];
      uint64_t seed = static_cast<uint64_t>(t) * 2654435761u + 1;
      WriteOptions wo;
      wo.sync = c.sync;
      const int per = c.num / c.threads;
      for (int i = 0; i < per; ++i) {
        const uint64_t n = sequential ? static_cast<uint64_t>(t) * per + i
                                      : (seed = seed * 6364136223846793005ull + 1442695040888963407ull) >> 20;
        const std::string k = MakeKey(c, n % static_cast<uint64_t>(c.num));
        const uint64_t t0 = NowMicros();
        Status s = db->Put(wo, Slice(k), Slice(value));
        r.hist.Add(NowMicros() - t0);
        if (s.ok()) ++r.ops;
      }
    });
  }
  for (auto& th : ts) th.join();
  Report(c, results, std::chrono::duration<double>(Clock::now() - start).count(), db);
}

void RunRead(const Config& c, DB* db, bool hot, bool absent) {
  std::vector<ThreadResult> results(static_cast<size_t>(c.threads));
  const auto start = Clock::now();
  std::vector<std::thread> ts;
  for (int t = 0; t < c.threads; ++t) {
    ts.emplace_back([&, t] {
      ThreadResult& r = results[static_cast<size_t>(t)];
      uint64_t seed = static_cast<uint64_t>(t) * 88172645463325252ull + 7;
      std::string v;
      const int per = c.num / c.threads;
      for (int i = 0; i < per; ++i) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        uint64_t n = seed % static_cast<uint64_t>(c.num);
        if (hot) n = HotKey(seed, static_cast<uint64_t>(c.num));
        // ABSENT KEYS MUST FALL INSIDE THE KEY RANGE, or this benchmark measures the
        // wrong filter. Probing "absent123" against a database of "key000..." keys sorts
        // below every file's smallest key, so SPEC 3.7's per-file RANGE check rejects it
        // before the Bloom filter is ever consulted -- `bloom-checked=0` is how that
        // showed up. The preload writes only EVEN keys and this probes ODD ones, so every
        // probe is in range and the filter is what does the work. See CHALLENGES B20.
        const std::string k = absent ? MakeKey(c, n | 1) : MakeKey(c, n);
        const uint64_t t0 = NowMicros();
        Status s = db->Get(ReadOptions(), Slice(k), &v);
        r.hist.Add(NowMicros() - t0);
        ++r.ops;
        if (s.ok()) ++r.found;
      }
    });
  }
  for (auto& th : ts) th.join();
  Report(c, results, std::chrono::duration<double>(Clock::now() - start).count(), db);
}

// SPEC 10.1's open-loop protocol. Latency is measured from the INTENDED start time, so a
// stall is attributed to every operation it delayed rather than silently reducing the
// offered load.
void RunOpenLoopWrite(const Config& c, DB* db) {
  std::vector<ThreadResult> results(static_cast<size_t>(c.threads));
  const std::string value(static_cast<size_t>(c.value_size), 'v');
  const double per_thread_rate = c.open_loop_rate / c.threads;
  const uint64_t interval_us = static_cast<uint64_t>(1e6 / per_thread_rate);
  const auto start = Clock::now();
  const uint64_t t_start = NowMicros();

  std::vector<std::thread> ts;
  for (int t = 0; t < c.threads; ++t) {
    ts.emplace_back([&, t] {
      ThreadResult& r = results[static_cast<size_t>(t)];
      WriteOptions wo;
      wo.sync = c.sync;
      uint64_t seed = static_cast<uint64_t>(t) * 2654435761u + 1;
      const int per = c.num / c.threads;
      for (int i = 0; i < per; ++i) {
        const uint64_t intended = t_start + static_cast<uint64_t>(i) * interval_us;
        const uint64_t now = NowMicros();
        if (now < intended) {
          std::this_thread::sleep_for(std::chrono::microseconds(intended - now));
        } else if (now - intended > 1000000) {
          // More than a second behind schedule: the offered rate exceeds what the engine
          // sustains, so the backlog is growing without bound and every later percentile
          // is a function of run DURATION rather than of the engine.
          ++r.divergence;
        }
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        const std::string k = MakeKey(c, (seed >> 20) % static_cast<uint64_t>(c.num));
        Status s = db->Put(wo, Slice(k), Slice(value));
        r.hist.Add(NowMicros() - intended);   // <-- from INTENDED start, not from issue
        if (s.ok()) ++r.ops;
      }
    });
  }
  for (auto& th : ts) th.join();
  Report(c, results, std::chrono::duration<double>(Clock::now() - start).count(), db);
}

void RunMixed(const Config& c, DB* db) {
  std::vector<ThreadResult> results(static_cast<size_t>(c.threads + c.reads));
  std::atomic<bool> stop{false};
  const std::string value(static_cast<size_t>(c.value_size), 'v');
  const auto start = Clock::now();

  std::vector<std::thread> ts;
  for (int t = 0; t < c.threads; ++t)
    ts.emplace_back([&, t] {
      ThreadResult& r = results[static_cast<size_t>(t)];
      uint64_t seed = static_cast<uint64_t>(t) + 11;
      WriteOptions wo;
      wo.sync = c.sync;
      while (!stop.load(std::memory_order_acquire)) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        const std::string k = MakeKey(c, (seed >> 20) % static_cast<uint64_t>(c.num));
        const uint64_t t0 = NowMicros();
        db->Put(wo, Slice(k), Slice(value));
        r.hist.Add(NowMicros() - t0);
        ++r.ops;
      }
    });
  for (int t = 0; t < c.reads; ++t)
    ts.emplace_back([&, t] {
      ThreadResult& r = results[static_cast<size_t>(c.threads + t)];
      uint64_t seed = static_cast<uint64_t>(t) * 99 + 5;
      std::string v;
      while (!stop.load(std::memory_order_acquire)) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        const std::string k = MakeKey(c, HotKey(seed, static_cast<uint64_t>(c.num)));
        const uint64_t t0 = NowMicros();
        Status s = db->Get(ReadOptions(), Slice(k), &v);
        r.hist.Add(NowMicros() - t0);
        ++r.ops;
        if (s.ok()) ++r.found;
      }
    });

  std::this_thread::sleep_for(std::chrono::seconds(c.seconds ? c.seconds : 10));
  stop.store(true, std::memory_order_release);
  for (auto& th : ts) th.join();
  Report(c, results, std::chrono::duration<double>(Clock::now() - start).count(), db);
}

// SPEC 2.1's B-null baseline: raw write() + fsync() of the same bytes to a flat file, with
// no index and no structure. It is the FLOOR -- what the hardware and this container can
// do. The engine cannot beat it; the claim is that it gets close while providing an index,
// which is the entire point of an LSM tree.
void RunBaselineAppend(const Config& c) {
  Env* env = Env::Default();
  std::unique_ptr<WritableFile> f;
  const std::string path = c.dbdir + "/baseline.log";
  if (!env->NewWritableFile(path, &f).ok()) { std::printf("baseline: cannot create file\n"); return; }
  const std::string value(static_cast<size_t>(c.value_size), 'v');
  Histogram h;
  const auto start = Clock::now();
  for (int i = 0; i < c.num; ++i) {
    const std::string k = MakeKey(c, static_cast<uint64_t>(i));
    const uint64_t t0 = NowMicros();
    f->Append(Slice(k));
    f->Append(Slice(value));
    if (c.sync) f->Sync();
    h.Add(NowMicros() - t0);
  }
  f->Close();
  env->DeleteFile(path);
  const double s = std::chrono::duration<double>(Clock::now() - start).count();
  std::printf("\n=== B-null baseline (raw append%s, no index) ===\n", c.sync ? " + fsync" : "");
  std::printf("    %d ops in %.2fs -> %.0f ops/s   %s\n", c.num, s, c.num / s, h.Report().c_str());
}

}  // namespace

int main(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--bench") c.bench = val();
    else if (a == "--db") c.dbdir = val();
    else if (a == "--threads") c.threads = std::atoi(val().c_str());
    else if (a == "--num") c.num = std::atoi(val().c_str());
    else if (a == "--value_size") c.value_size = std::atoi(val().c_str());
    // val() ADVANCES i, so it must be called exactly once per flag. Writing
    // `(val() == "true" || val() == "1")` consumed two arguments and silently ate the next
    // flag -- which surfaced as "unknown flag /data/b" three flags later.
    else if (a == "--sync") { const std::string v = val(); c.sync = (v == "true" || v == "1"); }
    else if (a == "--bloom_bits") c.bloom_bits = std::atoi(val().c_str());
    else if (a == "--cache_shards") c.cache_shards = std::atoi(val().c_str());
    else if (a == "--cache_bytes") c.cache_bytes = static_cast<size_t>(std::atoll(val().c_str()));
    else if (a == "--write_buffer") c.write_buffer = static_cast<size_t>(std::atoll(val().c_str()));
    else if (a == "--tier_trigger") c.tier_trigger = std::atoi(val().c_str());
    else if (a == "--reads") c.reads = std::atoi(val().c_str());
    else if (a == "--rate") c.open_loop_rate = std::atof(val().c_str());
    else if (a == "--seconds") c.seconds = std::atoi(val().c_str());
    else if (a == "--label") c.label = val();
    else { std::fprintf(stderr, "unknown flag %s\n", a.c_str()); return 2; }
  }

  Options opt = MakeOptions(c);
  Env* env = Env::Default();
  env->CreateDir(c.dbdir);

  if (c.bench == "baseline") { RunBaselineAppend(c); return 0; }

  // A fresh database per run: a benchmark that inherits the previous run's tiers is
  // measuring history, not the workload.
  DestroyDB(c.dbdir, opt);
  DB* db = nullptr;
  Status s = DB::Open(opt, c.dbdir, &db);
  if (!s.ok()) { std::fprintf(stderr, "open: %s\n", s.ToString().c_str()); return 1; }

  const bool needs_preload =
      c.bench == "readrandom" || c.bench == "readhot" || c.bench == "readmissing" ||
      c.bench == "mixed";
  if (needs_preload) {
    Config fill = c;
    fill.threads = 1;
    fill.bench = "fillseq";
    // readmissing preloads EVEN keys only, so the odd keys it probes are absent but
    // in-range -- see the note in RunRead.
    const int step = (c.bench == "readmissing") ? 2 : 1;
    std::printf("(preloading %d keys, step %d...)\n", c.num / step, step);
    const std::string value(static_cast<size_t>(c.value_size), 'v');
    for (int i = 0; i < c.num; i += step)
      db->Put(WriteOptions(), Slice(MakeKey(c, static_cast<uint64_t>(i))), Slice(value));
    db->CompactRange(nullptr, nullptr);
  }

  if (c.open_loop_rate > 0) {
    PrintHeader(c, "OPEN LOOP");
    RunOpenLoopWrite(c, db);
  } else if (c.bench == "fillseq") { PrintHeader(c, "closed loop"); RunWrite(c, db, true); }
  else if (c.bench == "fillrandom") { PrintHeader(c, "closed loop"); RunWrite(c, db, false); }
  else if (c.bench == "readrandom") { PrintHeader(c, "closed loop"); RunRead(c, db, false, false); }
  else if (c.bench == "readhot") { PrintHeader(c, "closed loop"); RunRead(c, db, true, false); }
  else if (c.bench == "readmissing") { PrintHeader(c, "closed loop"); RunRead(c, db, false, true); }
  else if (c.bench == "mixed") { PrintHeader(c, "closed loop"); RunMixed(c, db); }
  else { std::fprintf(stderr, "unknown bench %s\n", c.bench.c_str()); delete db; return 2; }

  delete db;
  return 0;
}
