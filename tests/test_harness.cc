// Tests for the test harness itself. Not ceremony: if CHECK_EQ silently passed on
// unequal values, or the seed were not reproducible, every later suite would be
// measuring nothing. This is the one place where a bug is invisible to every other test.
#include "tests/test.h"

#include <string>

TEST(check_macros_compare_correctly) {
  CHECK(true);
  CHECK_EQ(1 + 1, 2);
  CHECK_NE(1, 2);
  CHECK_LT(1, 2);
  CHECK_LE(2, 2);
  CHECK_GT(3, 2);
  CHECK_GE(2, 2);
  CHECK_EQ(std::string("ab"), std::string("ab"));
}

TEST(context_stack_is_balanced_across_scopes) {
  CHECK_EQ(::testing::context_stack().size(), 0u);
  {
    TCTX("outer=" << 1);
    CHECK_EQ(::testing::context_stack().size(), 1u);
    {
      TCTX("inner=" << 2);
      CHECK_EQ(::testing::context_stack().size(), 2u);
    }
    CHECK_EQ(::testing::context_stack().size(), 1u);
  }
  CHECK_EQ(::testing::context_stack().size(), 0u);
}

TEST(rng_is_deterministic_for_a_seed) {
  ::testing::Rng a(12345), b(12345), c(54321);
  for (int i = 0; i < 100; ++i) CHECK_EQ(a.next(), b.next());
  ::testing::Rng d(12345), e(54321);
  bool differs = false;
  for (int i = 0; i < 100; ++i) if (d.next() != e.next()) differs = true;
  CHECK(differs);
  (void)c;
}

TEST(rng_below_stays_in_range_including_zero) {
  ::testing::Rng r(::testing::seed());
  for (int i = 0; i < 1000; ++i) CHECK_LT(r.below(7), 7u);
  CHECK_EQ(r.below(0), 0u);   // must not divide by zero
}

RUN_ALL()
