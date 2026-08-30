// Minimal dependency-free test harness.
//
// No gtest, on purpose: this project's whole claim is that it has no hidden dependencies
// (SPEC 1.1), and a framework you can read in one sitting is easier to trust than one you
// install. It gives exactly what SPEC 7 needs and nothing else:
//
//   TEST(name)            register a test
//   CHECK / CHECK_EQ ...  non-fatal assertion (test keeps running, failure recorded)
//   REQUIRE(cond)         fatal assertion (returns from the test -- use when continuing
//                         would segfault, e.g. after a null or a failed Open)
//   CHECK_OK(status)      assertion on a lsmeng::Status, printing ToString() on failure
//   TCTX(...)             scoped context string appended to any failure inside the scope.
//                         Turns "CHECK_EQ failed" into "CHECK_EQ failed [iter=417 n=8193]",
//                         which is the difference between a reproducible bug and a shrug.
//   testing::seed()       reproducible RNG seed, overridable via LSMENG_SEED, always
//                         printed -- SPEC S18.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct Test {
  const char* name;
  void (*fn)();
};

inline std::vector<Test>& registry() { static std::vector<Test> r; return r; }
inline int& failures() { static int f = 0; return f; }
inline std::vector<std::string>& context_stack() {
  static std::vector<std::string> c;
  return c;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

// Scoped context. RAII so an early `return` inside a test cannot leave a stale frame.
struct Context {
  explicit Context(std::string s) { context_stack().push_back(std::move(s)); }
  ~Context() { context_stack().pop_back(); }
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
};

inline void fail(const char* file, int line, const std::string& what) {
  ++failures();
  std::fprintf(stderr, "  FAIL %s:%d: %s", file, line, what.c_str());
  for (const auto& c : context_stack()) std::fprintf(stderr, " [%s]", c.c_str());
  std::fprintf(stderr, "\n");
}

// SPEC S18: every test using randomness prints its seed and honours LSMENG_SEED, so a
// failure found on the 40,000th random operation can be replayed exactly. A random test
// you cannot replay is a rumour, not a bug report.
inline uint64_t seed() {
  static uint64_t s = []() -> uint64_t {
    if (const char* e = std::getenv("LSMENG_SEED")) return std::strtoull(e, nullptr, 10);
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
  }();
  return s;
}

// A tiny deterministic PRNG. Using std::mt19937_64 would be fine, but xorshift is three
// lines and makes the test corpus byte-identical across libstdc++ versions -- which
// matters when a seed is quoted in CHALLENGES.md and has to reproduce months later.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed_) : s(seed_ ? seed_ : 0x9E3779B97F4A7C15ull) {}
  uint64_t next() {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
  }
  uint32_t below(uint32_t n) { return n ? static_cast<uint32_t>(next() % n) : 0; }
  bool one_in(uint32_t n) { return below(n) == 0; }
};

// Stream-insert a value if it is printable, otherwise print its bytes. Never fails to
// compile, so CHECK_EQ works on arbitrary types.
template <typename T>
inline auto to_string_impl(const T& v, int) -> decltype(std::declval<std::ostringstream&>() << v, std::string()) {
  std::ostringstream os; os << v; return os.str();
}
template <typename T>
inline std::string to_string_impl(const T&, long) { return "<unprintable>"; }
template <typename T>
inline std::string str(const T& v) { return to_string_impl(v, 0); }

inline int run_all(const char* suite) {
  std::fprintf(stderr, "== %s == (LSMENG_SEED=%llu)\n", suite,
               static_cast<unsigned long long>(seed()));
  int ran = 0;
  for (const auto& t : registry()) {
    int before = failures();
    std::fprintf(stderr, "-- %s\n", t.name);
    t.fn();
    ++ran;
    if (failures() != before) std::fprintf(stderr, "   ^ %s FAILED\n", t.name);
  }
  std::fprintf(stderr, "== %s: %d test(s), %d failure(s)%s\n", suite, ran, failures(),
               failures() ? "" : "  OK");
  return failures() ? 1 : 0;
}

}  // namespace testing

#define LSMENG_CAT2(a, b) a##b
#define LSMENG_CAT(a, b) LSMENG_CAT2(a, b)

#define TEST(name)                                                    \
  static void name();                                                 \
  static ::testing::Registrar LSMENG_CAT(reg_, name)(#name, name);    \
  static void name()

#define TCTX(expr)                                                    \
  ::testing::Context LSMENG_CAT(ctx_, __LINE__)(                       \
      [&] { std::ostringstream os_; os_ << expr; return os_.str(); }())

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) ::testing::fail(__FILE__, __LINE__, "CHECK(" #cond ")");    \
  } while (0)

#define REQUIRE(cond)                                                             \
  do {                                                                            \
    if (!(cond)) {                                                                \
      ::testing::fail(__FILE__, __LINE__, "REQUIRE(" #cond ") -- aborting test"); \
      return;                                                                     \
    }                                                                             \
  } while (0)

#define LSMENG_CMP(a, b, op, label)                                            \
  do {                                                                         \
    const auto& va_ = (a);                                                     \
    const auto& vb_ = (b);                                                     \
    if (!((va_)op(vb_))) {                                                     \
      ::testing::fail(__FILE__, __LINE__,                                      \
                      std::string(label) + "(" #a ", " #b ") -- lhs=" +        \
                          ::testing::str(va_) + " rhs=" + ::testing::str(vb_)); \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b) LSMENG_CMP(a, b, ==, "CHECK_EQ")
#define CHECK_NE(a, b) LSMENG_CMP(a, b, !=, "CHECK_NE")
#define CHECK_LT(a, b) LSMENG_CMP(a, b, <, "CHECK_LT")
#define CHECK_LE(a, b) LSMENG_CMP(a, b, <=, "CHECK_LE")
#define CHECK_GT(a, b) LSMENG_CMP(a, b, >, "CHECK_GT")
#define CHECK_GE(a, b) LSMENG_CMP(a, b, >=, "CHECK_GE")

// Status-aware assertions. SPEC S25: tests must never branch on ToString(); they branch
// on code(). ToString() appears only in the failure message, for a human.
#define CHECK_OK(st)                                                           \
  do {                                                                         \
    const auto st_ = (st);                                                     \
    if (!st_.ok())                                                             \
      ::testing::fail(__FILE__, __LINE__,                                      \
                      "CHECK_OK(" #st ") -- " + st_.ToString());               \
  } while (0)

#define REQUIRE_OK(st)                                                         \
  do {                                                                         \
    const auto st_ = (st);                                                     \
    if (!st_.ok()) {                                                           \
      ::testing::fail(__FILE__, __LINE__,                                      \
                      "REQUIRE_OK(" #st ") -- " + st_.ToString());             \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define CHECK_CODE(st, expected)                                               \
  do {                                                                         \
    const auto st_ = (st);                                                     \
    if (st_.code() != (expected))                                              \
      ::testing::fail(__FILE__, __LINE__,                                      \
                      "CHECK_CODE(" #st ", " #expected ") -- got " +           \
                          st_.ToString());                                     \
  } while (0)

#define RUN_ALL() int main() { return ::testing::run_all(__FILE__); }
