#include <gtest/gtest.h>

// Confirms the test binary links against GoogleTest and runs at all.
TEST(Smoke, BuildLinksAndRuns) {
  ASSERT_TRUE(true);
  ASSERT_EQ(2 + 2, 4);
}
