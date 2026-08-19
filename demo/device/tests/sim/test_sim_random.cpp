#include <gtest/gtest.h>
#include "sim_random.h"

TEST(SimRandom, SameSeedSameSequence)
{
    sim_random_t *r1 = sim_random_create(42);
    sim_random_t *r2 = sim_random_create(42);
    ASSERT_NE(r1, nullptr);
    ASSERT_NE(r2, nullptr);
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(sim_random_next(r1), sim_random_next(r2));
    sim_random_destroy(r1);
    sim_random_destroy(r2);
}

TEST(SimRandom, DifferentSeedsDiffer)
{
    sim_random_t *r1 = sim_random_create(1);
    sim_random_t *r2 = sim_random_create(2);
    ASSERT_NE(r1, nullptr);
    ASSERT_NE(r2, nullptr);
    bool any_diff = false;
    for (int i = 0; i < 16; i++) {
        if (sim_random_next(r1) != sim_random_next(r2))
            any_diff = true;
    }
    EXPECT_TRUE(any_diff);
    sim_random_destroy(r1);
    sim_random_destroy(r2);
}

TEST(SimRandom, AutoSeedNeverStuck)
{
    sim_random_t *r1 = sim_random_create(0);
    sim_random_t *r2 = sim_random_create(0);
    ASSERT_NE(r1, nullptr);
    ASSERT_NE(r2, nullptr);
    for (int i = 0; i < 16; i++) {
        EXPECT_NE(sim_random_next(r1), 0u);
        EXPECT_NE(sim_random_next(r2), 0u);
    }
    sim_random_destroy(r1);
    sim_random_destroy(r2);
}

TEST(SimRandom, InstancesAreIsolated)
{
    sim_random_t *r1 = sim_random_create(7);
    sim_random_t *r2 = sim_random_create(7);
    sim_random_t *r3 = sim_random_create(7);
    ASSERT_NE(r1, nullptr);
    ASSERT_NE(r2, nullptr);
    ASSERT_NE(r3, nullptr);
    uint32_t first1 = sim_random_next(r1);
    uint32_t first3 = sim_random_next(r3);
    EXPECT_EQ(first1, first3);
    /* r2 未被动过：同一 seed 首值必须与 r1/r3 相同（推进 r1/r3 不影响 r2） */
    EXPECT_EQ(sim_random_next(r2), first1);
    sim_random_destroy(r1);
    sim_random_destroy(r2);
    sim_random_destroy(r3);
}
